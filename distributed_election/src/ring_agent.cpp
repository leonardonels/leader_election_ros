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

  // ----------------------------------- Map Sharing Setup -----------------------------------
  // Reliable QoS for map sharing to ensure topology consistency
  rclcpp::QoS map_qos(1);
  map_qos.reliable();
  map_qos.keep_last(1);
  map_qos.transient_local(); // Late joiners get the map immediately!

  map_pub_ = this->create_publisher<std_msgs::msg::Int32MultiArray>("/election/map", map_qos);

  map_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
    "/election/map", 
    map_qos, 
    std::bind(&RingAgent::on_map_received, this, std::placeholders::_1));

  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * heartbeat_max_tick_ / 3),
    std::bind(&RingAgent::on_startup_timer, this));

  // will be started after the intial announcement
  heartbeat_timer_->cancel();

  // only the leader strats the heartbeat
  timer_->cancel();

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_activate(const rclcpp_lifecycle::State & state)
{  
  current_tick_ = 0;
  last_token_tick_ = 0;
  leader_id_ = -1;

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
  map_pub_.reset();
  map_sub_.reset();
  return SimpleAgent::on_cleanup(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RingAgent::on_shutdown(const rclcpp_lifecycle::State & state)
{
  startup_timer_.reset();
  token_pub_.reset();
  token_sub_.reset();
  map_pub_.reset();
  map_sub_.reset();
  return SimpleAgent::on_shutdown(state);
}

void RingAgent::on_startup_timer()
{
  if(election_ready_) {
    startup_timer_->cancel();
    last_heartbeat_tick_map_[id_] = 0;
    network_view_map_[id_] = 0;

    run_election_logic();
    heartbeat_timer_->reset();
  }else{
    announce_heartbeat();

    // double announce for redundancy
    announce_heartbeat();

    RCLCPP_INFO(get_logger(), "Agent %d started.", id_);
    election_ready_ = true;
  }
}

int RingAgent::get_successor()
{
  int min_id = id_;
  int successor_id = id_;
  for (const auto & entry : last_heartbeat_tick_map_) 
  {
    if (entry.first == id_) continue;

    int agent_id = entry.first;
    if (agent_id < min_id) {
      min_id = agent_id;
    }
    if (agent_id > id_) {
      if (successor_id == id_ || agent_id < successor_id) {
        successor_id = agent_id;
      }
    }
  }
  if (successor_id == id_) {
    return min_id;
  }
  return successor_id;
}

void RingAgent::publish_leader()
{
  std_msgs::msg::Int32 msg;
  msg.data = leader_id_;
  election_pub_->publish(msg);
}

// used once only by the leader
void RingAgent::publish_heartbeat()
{
  int successor = get_successor();
  if (successor == id_) { // am I alone?
    RCLCPP_WARN(get_logger(), "Agent %d has no known successors to send heartbeat to.", id_);
    return;
  }

  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(id_);
  msg.data.push_back(successor);
  heartbeat_pub_->publish(msg);
  // RCLCPP_INFO(get_logger(), "Agent %d sent heartbeat to Agent %d", id_, get_successor());
}

void RingAgent::forward_heartbeat(std_msgs::msg::Int32MultiArray msg)
{
  int successor = get_successor();
  if (successor == id_) { // am I alone?
    RCLCPP_WARN(get_logger(), "Agent %d has no known successors to forward heartbeat to.", id_);
    return;
  }
  msg.data.push_back(successor);
  heartbeat_pub_->publish(msg);
}

void RingAgent::forward_token(std_msgs::msg::Int32MultiArray msg)
{
  int successor = get_successor();
  if (successor == id_) { // am I alone?
    RCLCPP_WARN(get_logger(), "Agent %d has no known successors to forward election token to.", id_);
    return;
  }

  RCLCPP_DEBUG(get_logger(), "Agent %d forwarding election token to %d", id_, successor);
  msg.data.push_back(successor);
  token_pub_->publish(msg);
}

void RingAgent::announce_heartbeat()
{
  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(id_);
  heartbeat_pub_->publish(msg);
}

void RingAgent::share_map()
{
  std_msgs::msg::Int32MultiArray msg;
  for (const auto & entry : network_view_map_) 
  {
    msg.data.push_back(entry.first);
  }
  map_pub_->publish(msg);
  if (leader_id_ != id_) RCLCPP_INFO(get_logger(), "Agent %d shared global network map (%zu nodes).", id_, msg.data.size());
}

void RingAgent::on_map_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.empty()) return;

  for (const auto & agent_id : msg->data) 
  {
    if (network_view_map_.find(agent_id) == network_view_map_.end()) {
      network_view_map_[agent_id] = current_tick_;
      RCLCPP_INFO(get_logger(), "Agent %d updated network view: discovered Agent %d from received map.", id_, agent_id);
    }
  }

  // the leader could have lost track of some agents
  // if I my network view is wider than the leader's, inform it
  // this message comes from the leader or from another agent informing the leader
  if (leader_id_ != -1 && leader_id_ != id_) {
    int my_view_size = network_view_map_.size();
    int leader_view_size = msg->data.size();
    if (my_view_size > leader_view_size) {
      RCLCPP_INFO(get_logger(), "Agent %d noticed its network view (%d) is wider than leader Agent %d's view (%d). Sharing map again.",
                   id_, my_view_size, leader_id_, leader_view_size);
      share_map();
    }
  }
}

void RingAgent::revive_agent(int target_id)
{
  std_msgs::msg::Int32 msg;
  msg.data = target_id;
  revival_pub_->publish(msg);
}

void RingAgent::on_heartbeat()
{
  current_tick_++;
  // RCLCPP_INFO(get_logger(), "Agent %d heartbeat tick %d", id_, current_tick_);

  last_heartbeat_tick_map_[id_] = current_tick_;

  // if i'm the leader
  if (leader_id_ == id_) 
  {
    // check if successor is alive
    int successor = get_successor();
    if (last_heartbeat_tick_map_.find(successor) != last_heartbeat_tick_map_.end()) {
      if (current_tick_ - last_heartbeat_tick_map_[successor] > heartbeat_max_tick_) {
        RCLCPP_WARN(get_logger(), "Agent %d detected its successor Agent %d is dead. Removing from heartbeat map.", id_, successor);
        last_heartbeat_tick_map_.erase(successor);
      }
    }
    publish_heartbeat();
    share_map();

    // if someone from network view is missing in liveness map, revive them
    for (const auto & entry : network_view_map_) {
      int known_id = entry.first;
      if (known_id == id_) continue;
      if (last_heartbeat_tick_map_.find(known_id) == last_heartbeat_tick_map_.end()) {
         RCLCPP_WARN(get_logger(), "Agent %d (Leader) reviving missing Agent %d found in network map.", id_, known_id);
         revive_agent(known_id);
      }
    }
  // if i'm not the leader
  } else {
    // and i did not receive the token for a while, annoouce my heartbeat again
    if (current_tick_ - last_token_tick_ > heartbeat_max_tick_) {
      RCLCPP_WARN(get_logger(), "Agent %d did not receive token for a while. Re-announcing heartbeat.", id_);
      announce_heartbeat();
      last_token_tick_ = current_tick_;
    }

    // if i think that i should be the leader run new elections
    if(leader_id_ < id_) {
      RCLCPP_WARN(get_logger(), "Agent %d thinks should be the leader instead of Agent %d. Initiating election.", id_, leader_id_);
      run_election_logic(); 
      return;
    }

    // check liveness of the leader
    if (leader_id_ != -1) {
      if (last_heartbeat_tick_map_.find(leader_id_) == last_heartbeat_tick_map_.end()) {
        RCLCPP_WARN(get_logger(), "Agent %d has never received heartbeat from or lost rack of the leader Agent %d", id_, leader_id_);
        run_election_logic(); 
        return;
      }else if (current_tick_ - last_heartbeat_tick_map_[leader_id_] > heartbeat_max_tick_) {
        RCLCPP_WARN(get_logger(), "Agent %d detected failure of leader Agent %d", id_, leader_id_);
        last_heartbeat_tick_map_.erase(leader_id_);
        run_election_logic(); 
        return;
      }
    }
  }
    
  
}

void RingAgent::on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  // announce heartbeat
  if (msg->data.size() == 1)
  {
    // safety_check:
    if (msg->data[0] == -1) return; // invalid announce heartbeat
    // someone impersonating an agent with id_ -1 can be a security edge case, keep in mind...
    
    if (current_tick_ > 0 && 
        (last_heartbeat_tick_map_.find(msg->data[0]) == last_heartbeat_tick_map_.end() ||
         current_tick_ - last_heartbeat_tick_map_[msg->data[0]] > heartbeat_max_tick_))
    {
      announce_heartbeat();
      if (leader_id_ == id_) {
        publish_leader();
      }
    }

    last_heartbeat_tick_map_[msg->data[0]] = current_tick_;
    // could represent a message from a new agent,
    // from a freshly revived agent
    // or from an agent which is still alive but noticed was wrongly skipped


  // standard heartbeat
  }else{
    if (!election_ready_ || current_tick_ <= 0) return;

    int msg_size = msg->data.size();
    // it's for me
    if (msg->data[msg_size - 1] == id_) 
    {
      last_token_tick_ = current_tick_;

      // update map based on heartbeat
      for (int i = 0; i < msg_size - 1; ++i) { //msg_size -1 it's me, it's already updated in on_heartbeat
        last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
      }

      // if i'm not the leader forward the heartbeat to my successor
      if (leader_id_ != id_) {
        // forward to successor if i'm not already on the list
        if (std::find(msg->data.begin(), msg->data.end() -1, id_) != msg->data.end() -1) {
          RCLCPP_WARN(get_logger(), "Agent %d received heartbeat but is already in the list. Ignoring.", id_);
          return;
        }
        // check if my successor is alive
        int successor = get_successor();
        if (successor == id_) { // am I alone?
          RCLCPP_WARN(get_logger(), "Agent %d has no known successors to forward heartbeat to.", id_);
          return;
        }
        if (last_heartbeat_tick_map_.find(successor) != last_heartbeat_tick_map_.end()) {
          if (current_tick_ - last_heartbeat_tick_map_[successor] > heartbeat_max_tick_) {
            RCLCPP_WARN(get_logger(), "Agent %d detected its successor Agent %d is dead. Removing from heartbeat map.", id_, successor);
            last_heartbeat_tick_map_.erase(successor);
          }
        }
        forward_heartbeat(*msg);
      
      // I'm the leader compare the token with the network view
      } else {
        if (msg_size -1 < int(network_view_map_.size())) {
          RCLCPP_WARN(get_logger(), "Agent %d (Leader) detected incomplete heartbeat ring.", id_);
          // emergency shortcut to revive missing leaders
          int possible_missing_leader = *std::max_element(msg->data.begin(), msg->data.end() -1);
          if (possible_missing_leader != leader_id_) {
            RCLCPP_WARN(get_logger(), "Agent %d (Leader) reviving missing leader Agent %d found in heartbeat ring.", id_, possible_missing_leader);
            revive_agent(possible_missing_leader);
          } 

          // even if i found the true leader, let's help him out with the missing nodes
          for (const auto & entry : network_view_map_) {
            int known_id = entry.first;
            if (known_id == id_) continue;
            if (known_id == possible_missing_leader) continue;  // already handled
            if (std::find(msg->data.begin(), msg->data.end() -1, known_id) == msg->data.end() -1) {
               RCLCPP_WARN(get_logger(), "Agent %d (Leader) reviving missing Agent %d found in network map.", id_, known_id);
               revive_agent(known_id);
            }
          }
        }
      }

    // it's not for me
    } else {
      // SNOOPING & REPAIR: Check if I was skipped
      // If Sender sends to Target, and I am logically between them, I've been forgotten.
      int sender_id = msg->data[msg_size - 2];
      int target_id = msg->data[msg_size - 1];
      
      // Logic: Is 'id_' inside the arc (sender_id, target_id)?
      bool skipped = false;
      if (sender_id < target_id) {
        // Normal case: 1 -> 5. Skipped if 1 < id < 5
        if (id_ > sender_id && id_ < target_id) skipped = true;
      } else {
        // Wrap around: 5 -> 1. Skipped if id > 5 OR id < 1
        if (id_ > sender_id || id_ < target_id) skipped = true;
      }
      if (skipped) {
          RCLCPP_WARN(get_logger(), "Agent %d detected it was skipped in heartbeat from %d to %d. Re-announcing.", id_, sender_id, target_id);
          announce_heartbeat();
      }

      // update map based on snoop
      for (int i = 0; i < msg_size -1; ++i) {
        last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
      }
    }
  }
}

// technically is not the hole logic of the ring election, but wan't renamed for convenience
void RingAgent::run_election_logic()
{  
  RCLCPP_INFO(get_logger(), "Agent %d initiating Ring Election", id_);
  publish_heartbeat(); // ensure everyone knows I'm alive
  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(id_);
  int successor = get_successor();
  msg.data.push_back(successor);
  token_pub_->publish(msg);
}

void RingAgent::on_token_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.size() < 2) return; // invalid token

  // The standard Ring optimization (Chang-Roberts algorithm principle) is to suppress elections initiated by agents with a lower ID than yourself.
  if (msg->data[0] < id_) {
    RCLCPP_DEBUG(get_logger(), "Agent %d suppressing election from lower priority Agent %d", id_, msg->data[0]);
    return;
  }

  // it's for me
  if (msg->data[msg->data.size() -1] == id_)
  {
    // Update liveness map for nodes in the token path
    // This allows us to learn that nodes are alive even during an election
    for (size_t i = 0; i < msg->data.size() - 1; ++i) {
      last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
    }

    // i'm the initiator -> ring concluded
    if (msg->data[0] == id_) 
    {
      // ring concluded
      int new_leader = *std::max_element(msg->data.begin(), msg->data.end() -1);
      RCLCPP_INFO(get_logger(), "Agent %d concluded election. New leader is Agent %d", id_, new_leader);
      leader_id_ = new_leader;
      publish_leader();

    // i'm not the initiator -> forward to successor
    } else {
      if (std::find(msg->data.begin(), msg->data.end() -1, id_) != msg->data.end() -1) {
        RCLCPP_WARN(get_logger(), "Agent %d received election token but is already in the list. Ignoring.", id_);
        return;
      }
      forward_token(*msg);
    }

  // it's not for me
  } else {
    // check if i've been skipped
    // should make a scene only if my id is the highest (eg.: 8 -> 0, I'm 9)
    int sender_id = msg->data[msg->data.size() - 2];;
    int target_id = msg->data[msg->data.size() - 1];;
    if (sender_id < id_ && id_ < target_id) {
      RCLCPP_WARN(get_logger(), "Agent %d detected it was skipped in election token from %d to %d. Re-initiating election.", id_, sender_id, target_id);
      // no need to specify anything, just start a new election
      // if i'm the highest, my election will propagate correctly
      // if not, my election will be suppressed by the rightful initiator
      run_election_logic();
      return;
    }
    // snoop and update map?
    // it's not necessary, but is still useful information
    for (size_t i = 0; i < msg->data.size() -1; ++i) {
      last_heartbeat_tick_map_[msg->data[i]] = current_tick_;
    }
  }
}

void RingAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  if (leader_id_ == msg->data) 
  {
    // Optimistic Upate: Assume the new leader is alive right now.
    // This prevents the on_heartbeat() check from declaring the new leader dead 
    // in the split second before the first actual heartbeat packet arrives.
    last_heartbeat_tick_map_[leader_id_] = current_tick_;
    return;  
  }

  leader_id_ = msg->data;
  RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);

  // Optimistic Upate: Assume the new leader is alive right now.
  // This prevents the on_heartbeat() check from declaring the new leader dead 
  // in the split second before the first actual heartbeat packet arrives.
  last_heartbeat_tick_map_[leader_id_] = current_tick_;

  // safety measure: if I'm the leader, restart heartbeat
  if (leader_id_ == id_) {
    announce_heartbeat();
  }
}

}  // namespace distributed_election
