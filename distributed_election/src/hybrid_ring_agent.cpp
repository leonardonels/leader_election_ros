#include "distributed_election/hybrid_ring_agent.hpp"
#include <algorithm>

namespace distributed_election
{

HybridRingAgent::HybridRingAgent(const std::string & node_name, int id, int heartbeat_interval_ms)
: SimpleAgent(node_name, id, heartbeat_interval_ms),
  election_ready_(false)
{
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
HybridRingAgent::on_configure(const rclcpp_lifecycle::State & state)
{
  // ----------------------------------- Simple Agent Setup -----------------------------------
  auto result = SimpleAgent::on_configure(state);
  if (result != rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS) {
    return result;
  }

  // ----------------------------------- Ring Agent Setup -----------------------------------
  rclcpp::QoS qos_profile(1);
  qos_profile.best_effort();
  
  token_pub_ = this->create_publisher<std_msgs::msg::Int32MultiArray>("/election/ring_token", qos_profile);

  token_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
    "/election/ring_token", 
    qos_profile, 
    std::bind(&HybridRingAgent::on_token_received, this, std::placeholders::_1));

  map_pub_ = this->create_publisher<std_msgs::msg::Int32MultiArray>("/election/map", qos_profile);

  map_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
    "/election/map", 
    qos_profile, 
    std::bind(&HybridRingAgent::on_map_received, this, std::placeholders::_1));

  gossip_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 5),
    std::bind(&HybridRingAgent::gossip_map, this));

  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 2),
    std::bind(&HybridRingAgent::on_startup_timer, this));

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

void HybridRingAgent::gossip_map()
{
  if (leader_id_ == id_) {
    return;
  }
  std_msgs::msg::Int32MultiArray msg;
  msg.data.clear();
  for (const auto & entry : last_heartbeat_map_) {
    msg.data.push_back(entry.first);
  }
  map_pub_->publish(msg);
}

void HybridRingAgent::on_map_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (leader_id_ != id_) {
    return;
  }
  for (const auto & agent_id : msg->data) {
    if (last_heartbeat_map_.find(agent_id) == last_heartbeat_map_.end()) {
      RCLCPP_INFO(get_logger(), "Leader %d adding Agent %d to network map", id_, agent_id);
      last_heartbeat_map_[agent_id] = this->now();
    }
  }
}

void HybridRingAgent::on_startup_timer()
{
  startup_timer_->cancel();
  election_ready_ = true;
  RCLCPP_INFO(get_logger(), "Startup delay finished. Ring Agent ready.");
}

void HybridRingAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  leader_id_ = msg->data;
  known_candidates_.clear();
  RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);
}

int HybridRingAgent::get_successor()
{
  int successor = -1;
  int min_id = -1;
  
  rclcpp::Time now = this->now();

  for (const auto & entry : last_heartbeat_map_) {
    int other_id = entry.first;
    rclcpp::Time last_seen = entry.second;

    // Skip dead nodes
    if ((now - last_seen).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * 2) {
      continue;
    }
    
    // Track minimum ID for wrap-around
    if (min_id == -1 || other_id < min_id) {
      min_id = other_id;
    }

    // Find smallest ID greater than current ID
    if (other_id > id_) {
      if (successor == -1 || other_id < successor) {
        successor = other_id;
      }
    }
  }

  // If no successor found (end of ring), wrap around to min_id
  if (successor == -1) {
    successor = min_id;
  }
  
  // If still -1, we are alone
  if (successor == -1) {
    return id_;
  }

  return successor;
}

void HybridRingAgent::run_election_logic()
{  
  if (!election_ready_) {
    return;
  }
  
  RCLCPP_INFO(get_logger(), "Agent %d initiating Ring Election", id_);
  
  known_candidates_.clear();
  known_candidates_.insert(id_);

  int successor = get_successor();
  if (successor == id_) {
    // Alone, declare self leader
    std_msgs::msg::Int32 msg;
    msg.data = id_;
    election_pub_->publish(msg);
    return;
  }

  std_msgs::msg::Int32MultiArray token;
  token.data.push_back(successor); // Target ID at index 0
  token.data.push_back(id_);       // Candidate list starts
  
  token_pub_->publish(token);
}

void HybridRingAgent::on_token_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.empty()) return;

  int target_id = msg->data[0];
  if (target_id != id_) {
    return; // Not for me
  }

  std::vector<int> candidates(msg->data.begin() + 1, msg->data.end());
  
  // Aggregation Logic:
  // 1. Check if received candidates are a subset of what we already know.
  //    If so, we have already forwarded a superset token, so we can drop this one.
  bool is_subset = true;
  for (int c : candidates) {
    if (known_candidates_.find(c) == known_candidates_.end()) {
      is_subset = false;
      break;
    }
  }

  if (is_subset && !known_candidates_.empty()) {
    RCLCPP_INFO(get_logger(), "Dropping redundant election token (subset of known candidates).");
    return;
  }

  // 2. Merge candidates
  for (int c : candidates) {
    known_candidates_.insert(c);
  }
  known_candidates_.insert(id_);

  // 3. Check if I was ALREADY in the incoming token (Circuit Complete)
  bool circuit_complete = false;
  for (int candidate : candidates) {
    if (candidate == id_) {
      circuit_complete = true;
      break;
    }
  }

  if (circuit_complete) {
    // Election finished, find max ID
    int new_leader = id_;
    for (int candidate : candidates) {
      if (candidate > new_leader) {
        new_leader = candidate;
      }
    }
    
    // Any node that detects circuit completion announces the result
    RCLCPP_INFO(get_logger(), "Ring circuit complete. New Leader: %d", new_leader);
    std_msgs::msg::Int32 leader_msg;
    leader_msg.data = new_leader;
    election_pub_->publish(leader_msg);
    known_candidates_.clear();
    
  } else {
    // Forward merged candidates
    int successor = get_successor();
    
    std_msgs::msg::Int32MultiArray new_token;
    new_token.data.push_back(successor); // Target
    for (int c : known_candidates_) {
      new_token.data.push_back(c);
    }
    
    RCLCPP_INFO(get_logger(), "Forwarding aggregated election token to %d", successor);
    token_pub_->publish(new_token);
  }
}

}  // namespace distributed_election
