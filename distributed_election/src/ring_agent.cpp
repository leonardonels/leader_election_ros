#include "distributed_election/ring_agent.hpp"
#include <algorithm>

namespace distributed_election
{

RingAgent::RingAgent(const std::string & node_name, int id, int heartbeat_interval_ms, int heartbeat_max_tick)
: SimpleAgent(node_name, id, heartbeat_interval_ms),
  heartbeat_max_tick_(heartbeat_max_tick),
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

  // Allow longer startup time for network stabilization (10 heartbeat cycles)
  // With 100ms heartbeat, this gives 1 second for all nodes to discover each other
  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 10),
    std::bind(&RingAgent::on_startup_timer, this));

  watchdog_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * heartbeat_max_tick_),
    std::bind(&RingAgent::on_watchdog_timeout, this));
  watchdog_timer_->cancel(); // Start only when expecting a forward

  election_watchdog_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * heartbeat_max_tick_),
    std::bind(&RingAgent::on_election_watchdog_timeout, this));
  election_watchdog_timer_->cancel();

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
  if (election_watchdog_timer_) election_watchdog_timer_->cancel();
  if (startup_timer_) startup_timer_->cancel();
  return SimpleAgent::on_deactivate(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_cleanup(const rclcpp_lifecycle::State & state)
{
  watchdog_timer_.reset();
  election_watchdog_timer_.reset();
  startup_timer_.reset();
  token_pub_.reset();
  token_sub_.reset();
  return SimpleAgent::on_cleanup(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_shutdown(const rclcpp_lifecycle::State & state)
{
  watchdog_timer_.reset();
  election_watchdog_timer_.reset();
  startup_timer_.reset();
  token_pub_.reset();
  token_sub_.reset();
  return SimpleAgent::on_shutdown(state);
}

void RingAgent::on_startup_timer()
{
  if (!election_ready_)
  {
    election_ready_ = true;
    last_token_tick_ = 0;
    current_tick_ = 0;
    leader_id_ = -1;

    // Announce self to allow others to build map
    std_msgs::msg::Int32MultiArray msg;
    msg.data.push_back(id_);
    heartbeat_pub_->publish(msg);

    RCLCPP_INFO(get_logger(), "Agent %d is now ready for elections.", id_);
  }else{
    startup_timer_->cancel();
    if (leader_id_ == -1) {
      // RCLCPP_INFO(get_logger(), "Agent %d starting as leader.", id_);
      run_election_logic();
    }
  }
}

void RingAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  leader_id_ = msg->data;
  election_watchdog_timer_->cancel();
  
  RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);
}

void RingAgent::publish_heartbeat()
{
  if (!election_ready_) return;

  if (heartbeat_pub_->is_activated() && leader_id_ == id_) {
    std_msgs::msg::Int32MultiArray msg;
    msg.data.push_back(id_);
    int successor = get_successor();
    msg.data.push_back(successor);
    heartbeat_pub_->publish(msg);
    // RCLCPP_DEBUG(get_logger(), "Agent %d sent heartbeat", id_);

    pending_token_ = msg;
    
    // Start watchdog for successor
    // If successor is leader, they won't forward (they consume), so don't watch.
    if (successor != leader_id_) {
      monitored_successor_ = successor;
      if (watchdog_timer_) {
        watchdog_timer_->reset();
      }
    } else {
      if (watchdog_timer_) watchdog_timer_->cancel();
    }
  }
}

void RingAgent::on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.size() < 1) return; // not valid

  // Safeguard for lost messages
  if (watchdog_timer_ && !watchdog_timer_->is_canceled()) {
    auto it = std::find(msg->data.begin(), msg->data.end(), monitored_successor_);
    // If successor is in the list and NOT the last element, they forwarded it.
    if (it != msg->data.end() && (it + 1) != msg->data.end()) {
      // RCLCPP_INFO(get_logger(), "Watchdog cancelled: Successor %d forwarded token to %d", monitored_successor_, *(it+1));
      watchdog_timer_->cancel();
    }
  }

  // if (msg->data.size() == 1) it corresponds to a node announcing itself
  if (msg->data.size() == 1) {
    last_heartbeat_tick_map_[msg->data[0]] = current_tick_;
    return;
  }

  // if (msg->data.size() >= 2) it corresponds to a ring heartbeat with every predecessor in it
  // meaning that i don't need to add myself since i'm the successor, but still need to add my successor
  int heartbeat_size = msg->data.size();
  if (msg->data[heartbeat_size - 1] == id_){  // it's for me

    // save a timestamp of the last token received
    last_token_tick_ = current_tick_;
    
    for (int i = 0; i < heartbeat_size; ++i) {  // last entry is myself -> it's ok
      last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
      //if (msg->data[i] == leader_id_) {
      //  // Update leader heartbeat time
      //  RCLCPP_INFO(get_logger(), "Agent %d received heartbeat from leader Agent %d", id_, leader_id_);
      //}
    }

    // if i'm the leader start a new heartbeat
    if (leader_id_ == id_){
      // Leader relies on the periodic timer to generate heartbeats.
      // Immediate regeneration here would cause token flooding.
      if (watchdog_timer_) watchdog_timer_->cancel();
      return;
    }
    
    // now let's continue the ring by sending to my successor
    int successor = get_successor();
    if (successor == id_) return; // am I alone?
    
    msg->data.push_back(successor);
    heartbeat_pub_->publish(*msg);
    pending_token_ = *msg;

    //RCLCPP_INFO(get_logger(), "Agent %d forwarding token to successor %d", id_, successor);
    
    // Start watchdog for successor
    // If successor is leader, they won't forward (they consume), so don't watch.
    if (successor != leader_id_) {
      monitored_successor_ = successor;
      if (watchdog_timer_) {
        watchdog_timer_->reset();
      }
    } else {
      if (watchdog_timer_) watchdog_timer_->cancel();
    }
  }else if (msg->data[heartbeat_size - 1] > id_){
    // is not for me, but it's an important information that cannot go to wasete
    for (int i = 0; i < heartbeat_size - 1; ++i) {  // leave out the last entry, i cannot know if it's alive, the leader should be at position 0
      if (msg->data[i] > id_) last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
    }
  }
}

void RingAgent::on_watchdog_timeout()
{
  if (watchdog_timer_) watchdog_timer_->cancel();
  RCLCPP_WARN(get_logger(), "Watchdog timeout! Successor %d failed to forward token.", monitored_successor_);
  
  // Mark successor as dead (locally) so get_successor skips it
  // Instead of erasing, we set its timestamp to 0 (beginning of epoch)
  last_heartbeat_tick_map_[monitored_successor_] = 0;
  
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
  current_tick_++;

  // if i'm the leader, no need to check if the leader is alive, but need to check every node and revive dead nodes
  if (leader_id_ == id_) {
    for (const auto & entry : last_heartbeat_tick_map_) {
      int other_id = entry.first;
      if (other_id == id_) continue;
      
      if (current_tick_ - entry.second > heartbeat_max_tick_ * int(last_heartbeat_tick_map_.size())) {
        RCLCPP_INFO(get_logger(), "Leader %d detected failure of Agent %d (tick diff: %d)", id_, other_id, current_tick_ - entry.second);
        std_msgs::msg::Int32 msg;
        msg.data = other_id;
        revival_pub_->publish(msg);
      }
    }
  }else{
    // each node verify with the heartbeat map if their leader is alive
    if (last_heartbeat_tick_map_.find(leader_id_) == last_heartbeat_tick_map_.end()) {
      // Never received heartbeat from leader
      RCLCPP_WARN(get_logger(), "Agent %d has never received heartbeat from leader Agent %d", id_, leader_id_);
      run_election_logic(); 
      return;
    }else if (current_tick_ - last_heartbeat_tick_map_[leader_id_] > heartbeat_max_tick_ * int(last_heartbeat_tick_map_.size())) {
      RCLCPP_WARN(get_logger(), "Agent %d detected failure of leader Agent %d (tick diff: %d)", id_, leader_id_, current_tick_ - last_heartbeat_tick_map_[leader_id_]);
      run_election_logic(); 
      return;
    }
    
    // each node verify how old is the newest heartbeat in the map (that corresponds to the last token seen)
    // if it is too old, assume that the leader is dead and start a new election
    //int successor = get_successor();
    //if (successor == id_) return; // am I alone?

    //if (current_tick_ - last_token_tick_ > timeout_time_) {
    //  RCLCPP_WARN(get_logger(), "No token received recently (tick diff: %d > timeout_time_: %d). Assuming leader %d is dead. Starting new election.", current_tick_ - last_token_tick_, timeout_time_, leader_id_);
    //  run_election_logic();
    //}
  }
}

int RingAgent::get_successor()
{
  int successor = -1;
  int min_id = -1;
  
  // Use last_heartbeat_tick_map_ to find dynamic successor
  for (const auto & entry : last_heartbeat_tick_map_) {
    int other_id = entry.first;
    if (other_id == id_) continue;

    // Check timestamp if not already confirmed alive
    if (current_tick_ - entry.second > heartbeat_max_tick_) {
      continue; // Skip dead nodes
    }

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
  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(id_);
  int successor = get_successor();
  msg.data.push_back(successor);
  token_pub_->publish(msg);

  pending_election_token_ = msg;
  monitored_election_successor_ = successor;
  if (election_watchdog_timer_) {
    election_watchdog_timer_->reset();
  }
}

void RingAgent::on_token_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  int token_size = msg->data.size();

  // if token_size < 2 it's not valid
  // if token.data[token_size -1] == id_ the ring is complete and i decide who is the leader
  // else if token.data[token_size -1] != id_ it's not for me, ignore
  // dead nodes are handled in on_heartbeat_received

  if (token_size < 2) return; // not valid
  if (msg->data[token_size - 1] != id_) return; // not for me

  // CRITICAL: Repair the ring topology using the election token.
  // Since heartbeats stopped, the map is stale/empty. We MUST use this token 
  // to rediscover that the initiator (and others) are alive, otherwise we can't close the ring.
  for (int i = 0; i < token_size - 1; ++i) {
    last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
  }

  if (msg->data[0] == id_)  // ring completed and I'm the initiator
  {
    int candidate_leader = *std::max_element(msg->data.begin(), msg->data.end() -1);
    RCLCPP_INFO(get_logger(), "Agent %d completed Ring Election. New leader is Agent %d", id_, candidate_leader);
    leader_id_ = candidate_leader;
    std_msgs::msg::Int32 leader_msg;
    leader_msg.data = candidate_leader;
    election_pub_->publish(leader_msg);
    if (election_watchdog_timer_) election_watchdog_timer_->cancel();
    return;
  }else{
    // Someone else started the election, just forward if i'm not in the list
    if (std::find(msg->data.begin(), msg->data.end() -1, id_) != msg->data.end() -1) {
      RCLCPP_INFO(get_logger(), "Agent %d already in Ring Election token, not forwarding.", id_);
      return;
    }
    // There can be more than one election at the same time, so we just append ourselves and forward
    int successor = get_successor();
    if (successor == id_) return; // am I alone?
    RCLCPP_INFO(get_logger(), "Agent %d forwarding Ring Election token to %d", id_, successor);
    msg->data.push_back(successor);
    token_pub_->publish(*msg);

    pending_election_token_ = *msg;
    monitored_election_successor_ = successor;
    if (election_watchdog_timer_) {
      election_watchdog_timer_->reset();
    }
  }
}

void RingAgent::on_election_watchdog_timeout()
{
  if (election_watchdog_timer_) election_watchdog_timer_->cancel();
  RCLCPP_WARN(get_logger(), "Election Watchdog timeout! Successor %d failed to forward election token.", monitored_election_successor_);
  
  // Mark successor as dead (locally)
  last_heartbeat_tick_map_[monitored_election_successor_] = 0;
  
  // Retry with new successor
  int new_successor = get_successor();
  if (new_successor == id_) return; // am I alone?
  
  RCLCPP_INFO(get_logger(), "Retrying election token forward to new successor %d", new_successor);
  
  int token_size = pending_election_token_.data.size();
  if (token_size < 1) {
    RCLCPP_ERROR(get_logger(), "No pending election token to forward!");
    return;
  }
  pending_election_token_.data[token_size - 1] = new_successor;
  token_pub_->publish(pending_election_token_);
  
  monitored_election_successor_ = new_successor;
  election_watchdog_timer_->reset();
}

}  // namespace distributed_election
