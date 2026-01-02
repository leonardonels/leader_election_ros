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

  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 2),
    std::bind(&RingAgent::on_startup_timer, this));

  watchdog_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 3),
    std::bind(&RingAgent::on_watchdog_timeout, this));
  watchdog_timer_->cancel(); // Start only when expecting a forward

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_activate(const rclcpp_lifecycle::State & state)
{
  SimpleAgent::on_activate(state);
  
  // Announce self to allow others to build map
  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(id_);
  heartbeat_pub_->publish(msg);
  
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_deactivate(const rclcpp_lifecycle::State & state)
{
  if (watchdog_timer_) watchdog_timer_->cancel();
  if (startup_timer_) startup_timer_->cancel();
  return SimpleAgent::on_deactivate(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_cleanup(const rclcpp_lifecycle::State & state)
{
  watchdog_timer_.reset();
  startup_timer_.reset();
  token_pub_.reset();
  token_sub_.reset();
  return SimpleAgent::on_cleanup(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_shutdown(const rclcpp_lifecycle::State & state)
{
  watchdog_timer_.reset();
  startup_timer_.reset();
  token_pub_.reset();
  token_sub_.reset();
  return SimpleAgent::on_shutdown(state);
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

// Ring heartbeat to reduce node computation
// Since the node will still read the message to verify it's the intended recipient the traffic overhead is still there
void RingAgent::publish_heartbeat()
{
  // Passive: Do nothing. Leader initiates cycle in run_health_check.
}

void RingAgent::on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.empty()) return;

  int sender_id = msg->data[0];

  // Watchdog Logic: If I sent to 'sender' (who is my successor), and now 'sender' is forwarding/responding, it's alive.
  if (sender_id == monitored_successor_) {
    watchdog_timer_->cancel();
  }

  // Case 1: Announcement [ID]
  if (msg->data.size() == 1) {
    last_heartbeat_map_[sender_id] = this->now();
    return;
  }

  // Case 2: Token [Sender, Target, ...Payload...]
  int target_id = msg->data[1];

  if (target_id == id_) {
    // It's for me.
    last_heartbeat_map_[sender_id] = this->now();

    // If I am leader and this is the return of the token
    if (leader_id_ == id_ && msg->data.size() > 2 && msg->data[2] == id_) {
       // Cycle Complete. Analyze payload.
       // Payload starts at index 2: [Leader, Node1, Node2...]
       std::set<int> active_nodes;
       for (size_t i = 2; i < msg->data.size(); ++i) {
         active_nodes.insert(msg->data[i]);
       }
       
       // Check for missing nodes from our known map
       for (const auto & entry : last_heartbeat_map_) {
         if (active_nodes.find(entry.first) == active_nodes.end() && entry.first != id_) {
           RCLCPP_WARN(get_logger(), "Leader detected node %d missing from ring cycle.", entry.first);
           std_msgs::msg::Int32 revive_msg;
           revive_msg.data = entry.first;
           revival_pub_->publish(revive_msg);
         }
       }
       
       // ACK the cycle completion so predecessor knows I'm alive
       std_msgs::msg::Int32MultiArray ack_msg;
       ack_msg.data.push_back(id_);
       heartbeat_pub_->publish(ack_msg);
       
       return;
    }

    // Forwarding Logic
    std_msgs::msg::Int32MultiArray new_token = *msg;
    new_token.data[0] = id_; // Sender is me
    
    // Append self to payload if not already there (it shouldn't be)
    new_token.data.push_back(id_);
    
    int successor = get_successor();
    new_token.data[1] = successor; // Target is successor
    
    heartbeat_pub_->publish(new_token);
    
    // Start Watchdog
    monitored_successor_ = successor;
    pending_token_ = new_token; // Save for retry
    watchdog_timer_->reset();
  }
}

void RingAgent::on_watchdog_timeout()
{
  watchdog_timer_->cancel();
  RCLCPP_WARN(get_logger(), "Watchdog timeout! Successor %d failed to forward token.", monitored_successor_);
  
  // Mark successor as dead (locally) so get_successor skips it
  // We can't remove from map easily as it's used for discovery, but we can set time to 0
  last_heartbeat_map_.erase(monitored_successor_);
  
  // Retry with new successor
  int new_successor = get_successor();
  if (new_successor == id_) return; // Alone
  
  RCLCPP_INFO(get_logger(), "Retrying token forward to new successor %d", new_successor);
  
  pending_token_.data[1] = new_successor;
  heartbeat_pub_->publish(pending_token_);
  
  monitored_successor_ = new_successor;
  watchdog_timer_->reset();
}

void RingAgent::run_health_check()
{
  // Leader initiates the heartbeat cycle
  if (leader_id_ == id_ && election_ready_) {
    int successor = get_successor();
    if (successor == id_) return;

    std_msgs::msg::Int32MultiArray token;
    token.data.push_back(id_);       // Sender
    token.data.push_back(successor); // Target
    token.data.push_back(id_);       // Payload Start (Leader)
    
    heartbeat_pub_->publish(token);
    
    monitored_successor_ = successor;
    pending_token_ = token;
    watchdog_timer_->reset();
  }
}

int RingAgent::get_successor()
{
  int successor = -1;
  int min_id = -1;
  
  // Use last_heartbeat_map_ to find dynamic successor
  for (const auto & entry : last_heartbeat_map_) {
    int other_id = entry.first;
    if (other_id == id_) continue;

    if (min_id == -1 || other_id < min_id) {
      min_id = other_id;
    }

    if (other_id > id_) {
      if (successor == -1 || other_id < successor) {
        successor = other_id;
      }
    }
  }

  if (successor == -1) successor = min_id;
  if (successor == -1) return id_;
  return successor;
}

void RingAgent::run_election_logic()
{  
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
