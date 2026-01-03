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
  startup_time = 3;
  health_time = 2;
  watchdog_time = health_time + 1;
  timeout_time = health_time * 3;

  // Re-create health check timer to respect the new health_time variable
  // SimpleAgent creates it with a hardcoded value, so we override it here.
  health_check_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * health_time),
    std::bind(&RingAgent::run_health_check, this));

  rclcpp::QoS qos_profile(1);
  qos_profile.best_effort();
  
  token_pub_ = this->create_publisher<std_msgs::msg::Int32MultiArray>("/election/ring_token", qos_profile);

  token_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
    "/election/ring_token", 
    qos_profile, 
    std::bind(&RingAgent::on_token_received, this, std::placeholders::_1));

  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * startup_time),
    std::bind(&RingAgent::on_startup_timer, this));

  watchdog_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * watchdog_time),
    std::bind(&RingAgent::on_watchdog_timeout, this));
  watchdog_timer_->cancel(); // Start only when expecting a forward

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_activate(const rclcpp_lifecycle::State & state)
{  
  return SimpleAgent::on_activate(state);
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
  // Initialize last_token_ to current time to avoid immediate timeout
  last_token_ = this->now();

  // Announce self to allow others to build map
  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(id_);
  heartbeat_pub_->publish(msg);

  run_election_logic();

  publish_heartbeat();

  RCLCPP_INFO(get_logger(), "Agent %d is now ready for elections.", id_);
}

void RingAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  leader_id_ = msg->data;
  known_candidates_.clear();
  RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);
}

void RingAgent::publish_heartbeat()
{
  if (!election_ready_) return;

  if (heartbeat_pub_->is_activated() && leader_id_ == id_) {
    std_msgs::msg::Int32MultiArray msg;
    msg.data.push_back(id_);
    msg.data.push_back(get_successor());
    heartbeat_pub_->publish(msg);
    // RCLCPP_DEBUG(get_logger(), "Agent %d sent heartbeat", id_);
  }
}

void RingAgent::on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.size() < 1) return; // not valid

  // if (msg->data.size() == 1) it corresponds to a node announcing itself
  if (msg->data.size() == 1) {
    last_heartbeat_map_[msg->data[0]] = this->now();
    return;
  }

  // if (msg->data.size() >= 2) it corresponds to a ring heartbeat with every predecessor in it
  // meaning that i don't need to add myself since i'm the successor, but still need to add my successor
  int heartbeat_size = msg->data.size();
  if (msg->data[heartbeat_size - 1] == id_){  // it's for me

    // save a timestamp of the last token received
    last_token_ = this->now();
    
    for (int i = 0; i < heartbeat_size; ++i) {  // last entry is myself
      last_heartbeat_map_[msg->data[i]] = this->now();
    }

    // if i'm the leader start a new heartbeat
    if (leader_id_ == id_){
      // Leader relies on the periodic timer to generate heartbeats.
      // Immediate regeneration here would cause token flooding.
      return;
    }
    
    // now let's continue the ring by sending to my successor
    int successor = get_successor();
    if (successor == id_) return; // am I alone?
    
    msg->data.push_back(successor);
    heartbeat_pub_->publish(*msg);
    pending_token_ = *msg;
    
    // Start watchdog for successor
    monitored_successor_ = successor;
    if (watchdog_timer_) {
      watchdog_timer_->reset();
    }
  }else if(msg->data[heartbeat_size - 1] == monitored_successor_){
    // Forwarded heartbeat from my predecessor to my monitored successor
    last_heartbeat_map_[msg->data[heartbeat_size - 1]] = this->now();
    
    // Stop watchdog as token successfully forwarded
    if (watchdog_timer_) watchdog_timer_->cancel();
  }
}

void RingAgent::on_watchdog_timeout()
{
  if (watchdog_timer_) watchdog_timer_->cancel();
  RCLCPP_WARN(get_logger(), "Watchdog timeout! Successor %d failed to forward token.", monitored_successor_);
  
  // Mark successor as dead (locally) so get_successor skips it
  // We can't remove from map easily as it's used for discovery, but we can set time to 0
  last_heartbeat_map_.erase(monitored_successor_);
  
  // Retry with new successor
  int new_successor = get_successor();
  if (new_successor == id_) return; // am I alone?
  
  RCLCPP_INFO(get_logger(), "Retrying token forward to new successor %d", new_successor);
  
  int token_size = pending_token_.data.size();
  if (token_size < 1) {
    RCLCPP_ERROR(get_logger(), "No pending token to forward!");
    return;
  }
  pending_token_.data[token_size - 1] = new_successor;
  heartbeat_pub_->publish(pending_token_);
  
  monitored_successor_ = new_successor;
  watchdog_timer_->reset();
}

void RingAgent::run_health_check()
{
  // if i'm the leader, no need to check if the leader is alive, but need to check every node and revive dead nodes
  if (leader_id_ == id_) {
    rclcpp::Time now = this->now();
    for (const auto & entry : last_heartbeat_map_) {
      int other_id = entry.first;
      if (other_id == id_) continue;
      if ((now - entry.second).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * health_time) {
        RCLCPP_WARN(get_logger(), "Leader %d detected failure of Agent %d", id_, other_id);
        std_msgs::msg::Int32 msg;
        msg.data = other_id;
        revival_pub_->publish(msg);
      }
    }
    return;
  }
  
  // each node verify how old is the newest heartbeat in the map (that corresponds to the last token seen)
  // if it is too old, assume that the leader is dead and start a new election
  int successor = get_successor();
  if (successor == id_) return; // am I alone?

  rclcpp::Time now = this->now();
  if (now - last_token_ > rclcpp::Duration::from_seconds(heartbeat_interval_ms_ * timeout_time / 1000.0)) {
    RCLCPP_WARN(get_logger(), "No token received recently. Assuming leader %d is dead. Starting new election.", leader_id_);
    run_election_logic();
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
