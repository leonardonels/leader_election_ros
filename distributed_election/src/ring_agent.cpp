#include "distributed_election/ring_agent.hpp"
#include <algorithm>

namespace distributed_election
{

RingAgent::RingAgent(const std::string & node_name, int id, int heartbeat_interval_ms)
: SimpleAgent(node_name, id, heartbeat_interval_ms),
  election_ready_(false)
{
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_configure(const rclcpp_lifecycle::State & state)
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
    std::bind(&RingAgent::on_token_received, this, std::placeholders::_1));

  map_pub_ = this->create_publisher<std_msgs::msg::Int32MultiArray>("/election/map", qos_profile);

  map_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
    "/election/map", 
    qos_profile, 
    std::bind(&RingAgent::on_map_received, this, std::placeholders::_1));

  gossip_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 5),
    std::bind(&RingAgent::gossip_map, this));

  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 2),
    std::bind(&RingAgent::on_startup_timer, this));

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

void RingAgent::gossip_map()
{
  // Ring-based Map: Pass a token with the full map to the successor
  int successor = get_successor();
  if (successor == id_) return; // Alone

  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(successor); // Target
  for (const auto & entry : last_heartbeat_map_) {
    msg.data.push_back(entry.first);
  }
  // Add self if not in map (should be there though)
  if (last_heartbeat_map_.find(id_) == last_heartbeat_map_.end()) {
    msg.data.push_back(id_);
  }
  
  map_pub_->publish(msg);
}

void RingAgent::on_map_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.empty()) return;
  
  int target_id = msg->data[0];
  if (target_id != id_) return; // Not for me

  // Update local map with received IDs
  for (size_t i = 1; i < msg->data.size(); ++i) {
    int agent_id = msg->data[i];
    if (last_heartbeat_map_.find(agent_id) == last_heartbeat_map_.end()) {
      RCLCPP_INFO(get_logger(), "Discovered Agent %d via Ring Map", agent_id);
      last_heartbeat_map_[agent_id] = this->now();
    }
  }
  
  // Note: We don't forward immediately here. The gossip_timer_ will handle the next forward step.
  // This prevents a storm.
}

void RingAgent::on_startup_timer()
{
  startup_timer_->cancel();
  election_ready_ = true;
  RCLCPP_INFO(get_logger(), "Startup delay finished. Ring Agent ready.");
}

void RingAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  leader_id_ = msg->data;
  known_candidates_.clear();
  RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);
}

void RingAgent::publish_heartbeat()
{
  // Suppress SimpleAgent heartbeat
  // Implement Ring-based heartbeat: Publish to /election/heartbeats but only for successor to check?
  // Actually, if we suppress the broadcast, we must ensure we still publish SOMETHING if we want to use the topic.
  // But the user said "suppress the heartbeat publisher from the simple publisher".
  // And "use the heartbeat topic to check for dead nodes in a ring way".
  
  // Let's publish our ID. This is what SimpleAgent does.
  // But maybe we change the frequency or condition?
  // If we just call the base implementation, we are not suppressing it.
  // If we do nothing, we don't use the topic.
  
  // Interpretation: We still publish, but we change the CHECK logic.
  // OR: We publish to a specific target? No, topic is global.
  
  // Let's assume we KEEP publishing (so others can see us), but we override the CHECK.
  // Wait, "suppress the heartbeat publisher" means DO NOT PUBLISH using the SimpleAgent mechanism.
  // Maybe we publish manually in a different timer?
  // Or maybe we don't publish at all?
  
  // If we don't publish, how does the successor check us?
  // Maybe we send a direct message?
  
  // Let's try to implement a "Ring Heartbeat" where we send a token to the successor?
  // But the user said "use the heartbeat topic".
  
  // Let's assume the user means: "Don't use the SimpleAgent's broadcast. Instead, send a directed heartbeat."
  // Since we can't easily direct on a shared topic without ID, maybe we publish [TargetID, MyID]?
  
  if (heartbeat_pub_->is_activated()) {
    std_msgs::msg::Int32 msg;
    msg.data = id_;
    heartbeat_pub_->publish(msg);
  }
}

void RingAgent::run_health_check()
{
  // 1. Leader Revival Logic (Restored)
  if (leader_id_ == id_) {
    rclcpp::Time now = this->now();
    for (const auto & agent : last_heartbeat_map_) {
      if (agent.first == id_) continue;
      if ((now - agent.second).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * 4) { // Increased tolerance for ring
        RCLCPP_WARN(get_logger(), "Leader %d detected failure of Agent %d", id_, agent.first);
        std_msgs::msg::Int32 msg;
        msg.data = agent.first;
        revival_pub_->publish(msg);
      }
    }
  }

  // 2. Ring-based health check: Only check PREDECESSOR.
  if (last_heartbeat_map_.empty()) return;
  
  int predecessor_id = -1;
  int max_id = -1;
  
  // Find predecessor
  for (const auto & entry : last_heartbeat_map_) {
    int other_id = entry.first;
    if (max_id == -1 || other_id > max_id) max_id = other_id;
    
    if (other_id < id_) {
      if (predecessor_id == -1 || other_id > predecessor_id) {
        predecessor_id = other_id;
      }
    }
  }
  
  if (predecessor_id == -1) predecessor_id = max_id; // Wrap around
  if (predecessor_id == id_) return; // Alone
  
  // Check predecessor health
  if (last_heartbeat_map_.find(predecessor_id) != last_heartbeat_map_.end()) {
     rclcpp::Time last_seen = last_heartbeat_map_[predecessor_id];
     if ((this->now() - last_seen).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * 4) {
       RCLCPP_WARN(get_logger(), "Agent %d detected failure of predecessor Agent %d", id_, predecessor_id);
       // If predecessor dies, the ring is broken. Initiate election to repair/re-elect.
       run_election_logic();
     }
  }
}

int RingAgent::get_successor()
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

void RingAgent::run_election_logic()
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

void RingAgent::on_token_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.empty()) return;

  int target_id = msg->data[0];
  if (target_id != id_) {
    return; // Not for me
  }

  std::vector<int> candidates(msg->data.begin() + 1, msg->data.end());
  
  // Aggregation Logic
  bool is_subset = true;
  for (int c : candidates) {
    if (known_candidates_.find(c) == known_candidates_.end()) {
      is_subset = false;
      break;
    }
  }

  if (is_subset && !known_candidates_.empty()) {
    RCLCPP_INFO(get_logger(), "Dropping redundant election token.");
    return;
  }

  for (int c : candidates) {
    known_candidates_.insert(c);
  }
  known_candidates_.insert(id_);

  // Check Circuit Complete
  bool circuit_complete = false;
  for (int candidate : candidates) {
    if (candidate == id_) {
      circuit_complete = true;
      break;
    }
  }

  if (circuit_complete) {
    int new_leader = id_;
    for (int candidate : candidates) {
      if (candidate > new_leader) {
        new_leader = candidate;
      }
    }
    
    RCLCPP_INFO(get_logger(), "Ring circuit complete. New Leader: %d", new_leader);
    std_msgs::msg::Int32 leader_msg;
    leader_msg.data = new_leader;
    election_pub_->publish(leader_msg);
    known_candidates_.clear();
    
  } else {
    int successor = get_successor();
    std_msgs::msg::Int32MultiArray new_token;
    new_token.data.push_back(successor);
    for (int c : known_candidates_) {
      new_token.data.push_back(c);
    }
    
    RCLCPP_INFO(get_logger(), "Forwarding aggregated election token to %d", successor);
    token_pub_->publish(new_token);
  }
}

}  // namespace distributed_election
