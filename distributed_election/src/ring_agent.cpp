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

  ping_pub_ = this->create_publisher<std_msgs::msg::Int32>("/election/ping", qos_profile);

  ping_sub_ = this->create_subscription<std_msgs::msg::Int32>(
    "/election/ping",
    qos_profile,
    std::bind(&RingAgent::on_ping_received, this, std::placeholders::_1));

  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * heartbeat_max_tick_),
    std::bind(&RingAgent::on_startup_timer, this));

  heartbeat_timer->cancel();

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
  if (startup_timer_) startup_timer_->cancel();
  return SimpleAgent::on_deactivate(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_cleanup(const rclcpp_lifecycle::State & state)
{
  startup_timer_.reset();
  token_pub_.reset();
  token_sub_.reset();
  ping_pub_.reset();
  ping_sub_.reset();
  return SimpleAgent::on_cleanup(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_shutdown(const rclcpp_lifecycle::State & state)
{
  startup_timer_.reset();
  token_pub_.reset();
  token_sub_.reset();
  ping_pub_.reset();
  ping_sub_.reset();
  return SimpleAgent::on_shutdown(state);
}

int RingAgent::get_successor()
{
  int successor = -1;
  int min_id = -1;
  
  // Use last_heartbeat_tick_map_ to find dynamic successor
  for (const auto & entry : last_heartbeat_tick_map_) {
    int other_id = entry.first;
    if (other_id == id_) continue;

    // Check timestamp - skip dead nodes
    if (current_tick_ - entry.second > heartbeat_max_tick_) {
      continue;
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
  //RCLCPP_INFO(get_logger(), "Agent %d selected Agent %d as successor.", id_, successor);
  return successor;
}

void RingAgent::publish_heartbeat()
{
  if (!election_ready_) return;

  if (leader_id_ == id_) {
    std_msgs::msg::Int32MultiArray msg;
    msg.data.push_back(id_);
    int successor = get_successor();
    msg.data.push_back(successor);
    heartbeat_pub_->publish(msg);
    monitored_successor_ = successor;
    // RCLCPP_INFO(get_logger(), "Leader %d forwarded heartbeat to Agent %d", id_, successor);
  }
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
    heartbeat_timer->reset();
  }
}

void RingAgent::on_heartbeat()
{
  current_tick_++;

  // if i'm leader, send heartbeat and check for dead members
  if (leader_id_ == id_) {
    publish_heartbeat();
  // i'm not the leader
  }else{
    // Check leader heartbeat
    if (leader_id_ != id_){
      if (last_heartbeat_tick_map_.find(leader_id_) == last_heartbeat_tick_map_.end()) {
        // Never received heartbeat from leader
        RCLCPP_WARN(get_logger(), "Agent %d has never received heartbeat from leader Agent %d", id_, leader_id_);
        run_election_logic(); 
        return;
      }else if (current_tick_ - last_heartbeat_tick_map_[leader_id_] > heartbeat_max_tick_) {
        // Leader considered dead
        RCLCPP_WARN(get_logger(), "Agent %d detected failure of leader Agent %d (tick diff: %d)", id_, leader_id_, current_tick_ - last_heartbeat_tick_map_[leader_id_]);
        run_election_logic(); 
        return;
      }
    } 
  }
}

void RingAgent::on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  // setup
  int msg_size = msg->data.size();
  
  // invalid message
  if (msg_size < 1) return;

  // local setup
  last_heartbeat_tick_map_[msg->data[0]] = current_tick_;

  // announcing message
  if (msg_size == 1)
  {
    if (!election_ready_ && last_heartbeat_tick_map_.find(msg->data[0]) == last_heartbeat_tick_map_.end()) {
      startup_timer_->reset();
    }
    last_heartbeat_tick_map_[msg->data[0]] = current_tick_;
    
    // If leader and this was a ping response, clear ping state
    if (leader_id_ == id_) {
      last_ping_tick_.erase(msg->data[0]);
    }
    return;
  }

  //standard message
  if (msg_size >= 2)
  {
    // for me
    if (msg->data[msg_size - 1] == id_)
    {
      // update map for all nodes in token
      for (int i = 0; i < msg_size - 1; ++i) {
        last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
      }

      // then forward if i'm not the initiator
      // if i'm the leader i will always be the initiator and vice versa
      // so msg->data[0] != id_ is the same as leader_id_ != id_
      if (msg->data[0] != id_) 
      {     
        int successor = get_successor();
        if (successor == id_) return; // am I alone?
  
        // RCLCPP_INFO(get_logger(), "Agent %d forwarding heartbeat to %d", id_, successor);
        // msg_size-1 is already my ID, just append successor
        msg->data.push_back(successor);      // Append successor
        heartbeat_pub_->publish(*msg);
        monitored_successor_ = successor;
        // RCLCPP_INFO(get_logger(), "Agent %d forwarded heartbeat to Agent %d", id_, successor);
      }
      // but if i'm the leader let's use this token to confirm alive nodes and revive dead ones
      else{
        for (const auto & entry : last_heartbeat_tick_map_) {
          int other_id = entry.first;
          if (other_id == id_) continue;

          // check who is missing from the token since has been skipped
          if (std::find(msg->data.begin(), msg->data.end() -1, other_id) == msg->data.end() -1) {
            // other_id is missing - verify it's really dead
            if (current_tick_ - entry.second > heartbeat_max_tick_) {
              // Check if we already pinged this node recently
              auto ping_it = last_ping_tick_.find(other_id);
              if (ping_it == last_ping_tick_.end()) {
                // Never pinged - send ping first
                RCLCPP_INFO(get_logger(), "Leader %d pinging Agent %d (tick diff: %d)", id_, other_id, current_tick_ - entry.second);
                std_msgs::msg::Int32 ping_msg;
                ping_msg.data = other_id;
                ping_pub_->publish(ping_msg);
                last_ping_tick_[other_id] = current_tick_;
              } else if (current_tick_ - ping_it->second > heartbeat_max_tick_) {
                // Pinged but no response - try revival
                RCLCPP_WARN(get_logger(), "Leader %d detected failure of Agent %d, attempting revival (tick diff: %d)", id_, other_id, current_tick_ - entry.second);
                std_msgs::msg::Int32 revival_msg;
                revival_msg.data = other_id;
                revival_pub_->publish(revival_msg);
                last_ping_tick_.erase(other_id); // Clear ping state after revival attempt
              }
            } else {
              // Node is back - clear ping state
              last_ping_tick_.erase(other_id);
            }
          } else {
            // Node is in token - clear ping state
            last_ping_tick_.erase(other_id);
          }
        }
      }
    // not for me
    }else{
      // Update map with ALL nodes visible in this token (except recipient)
      for (int i = 0; i < msg_size - 1; ++i) {
        last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
      }
    }
  }
}

void RingAgent::run_election_logic()
{  
  RCLCPP_INFO(get_logger(), "Agent %d initiating Ring Election", id_);
  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(id_);
  int successor = get_successor();
  msg.data.push_back(successor);
  token_pub_->publish(msg);
}

void RingAgent::on_token_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  int token_size = msg->data.size();

  if (token_size < 2) return; // invalid token

  if (msg->data[token_size - 1] == id_) // for me
  {
    // update map -  don't waste this information
    for (int i = 0; i < token_size - 1; ++i) {
      last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
    }
    
    // i'm the initiator
    if (msg->data[0] == id_) 
    {
      // ringe concluded
      int new_leader = *std::max_element(msg->data.begin(), msg->data.end() -1);
      RCLCPP_INFO(get_logger(), "Agent %d concluded election. New leader is Agent %d", id_, new_leader);
      leader_id_ = new_leader;
      std_msgs::msg::Int32 leader_msg;
      leader_msg.data = new_leader;
      election_pub_->publish(leader_msg);
    }else{
      // forward to successor if i'm not already on the list
      if (std::find(msg->data.begin(), msg->data.end() -1, id_) != msg->data.end() -1) {
        RCLCPP_WARN(get_logger(), "Agent %d received election token but is already in the list. Ignoring.", id_);
        return;
      }

      int successor = get_successor();
      if (successor == id_) return; // am I alone?

      RCLCPP_DEBUG(get_logger(), "Agent %d forwarding election token to %d", id_, successor);
      msg->data.push_back(successor);
      token_pub_->publish(*msg);
    }
  }else{  // not for me
    // listen for other senders and update map
    if (msg->data[token_size - 2] > id_) {
      last_heartbeat_tick_map_[msg->data[token_size - 2]] = current_tick_;
    }
  }
}

void RingAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  if (leader_id_ != msg->data) {
    leader_id_ = msg->data;
    
    RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);
  }
}

void RingAgent::on_ping_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  // Respond to ping if it's for me
  if (msg->data == id_) {
    RCLCPP_INFO(get_logger(), "Agent %d received ping, announcing presence", id_);
    std_msgs::msg::Int32MultiArray announce_msg;
    announce_msg.data.push_back(id_);
    heartbeat_pub_->publish(announce_msg);
  }
}

}  // namespace distributed_election
