#include "distributed_election/simple_agent.hpp"

namespace distributed_election
{

SimpleAgent::SimpleAgent(const std::string & node_name, int id, int heartbeat_interval_ms)
: rclcpp_lifecycle::LifecycleNode(node_name),
  id_(id),
  heartbeat_interval_ms_(heartbeat_interval_ms)
{
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_configure(const rclcpp_lifecycle::State &)
{
  // ----------------------------------- ROS 2 Topics Setup -----------------------------------
  RCLCPP_INFO(get_logger(), "Configuring agent %d", id_);
  rclcpp::QoS qos_profile(1); // Keep last 1
  qos_profile.best_effort();  // UDP-like behavior (faster, less overhead)

  heartbeat_pub_ = this->create_publisher<std_msgs::msg::Int32>("/election/heartbeats", qos_profile);
  
  timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_),
    std::bind(&SimpleAgent::publish_heartbeat, this));

  heartbeat_sub_ = this->create_subscription<std_msgs::msg::Int32>(
    "/election/heartbeats", 
    qos_profile, 
    std::bind(&SimpleAgent::on_heartbeat_received, this, std::placeholders::_1));

  election_pub_ = this->create_publisher<std_msgs::msg::Int32>("/election/leader", qos_profile);

  election_sub_ = this->create_subscription<std_msgs::msg::Int32>(
    "/election/leader", 
    qos_profile, 
    std::bind(&SimpleAgent::on_leader_received, this, std::placeholders::_1));

  revival_pub_ = this->create_publisher<std_msgs::msg::Int32>("/election/revive", qos_profile);

  health_check_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 3),
    std::bind(&SimpleAgent::run_health_check, this));

  // -----------------------------------------------------------------------------------

  run_election_logic();

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_activate(const rclcpp_lifecycle::State &)
{
  // RCLCPP_INFO(get_logger(), "Activating agent %d", id_);
  heartbeat_pub_->on_activate();
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_deactivate(const rclcpp_lifecycle::State &)
{
  // RCLCPP_INFO(get_logger(), "Deactivating agent %d", id_);
  heartbeat_pub_->on_deactivate();
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_cleanup(const rclcpp_lifecycle::State &)
{
  // RCLCPP_INFO(get_logger(), "Cleaning up agent %d", id_);
  heartbeat_pub_.reset();
  timer_.reset();
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_shutdown(const rclcpp_lifecycle::State &)
{
  // RCLCPP_INFO(get_logger(), "Shutting down agent %d", id_);
  heartbeat_pub_.reset();
  timer_.reset();
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

void SimpleAgent::publish_heartbeat()
{
  if (heartbeat_pub_->is_activated()) {
    std_msgs::msg::Int32 msg;
    msg.data = id_;
    heartbeat_pub_->publish(msg);
    // RCLCPP_DEBUG(get_logger(), "Agent %d sent heartbeat", id_);
  }
}

void SimpleAgent::on_heartbeat_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  // Update the timestamp for this agent ID
  last_heartbeat_map_[msg->data] = this->now();
}

void SimpleAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  leader_id_ = msg->data;
  if (leader_id_ >= id_){
    RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);
  }else{
    RCLCPP_WARN(get_logger(), "Agent %d received leader %d with lower ID! Taking over...", id_, leader_id_);
    leader_id_ = id_;
    std_msgs::msg::Int32 msg;
    msg.data = id_;
    election_pub_->publish(msg);
  }
}

void SimpleAgent::run_election_logic()
{  
  rclcpp::Time now = this->now();
  // test logic: broadcast bully
  for (const auto & entry : last_heartbeat_map_) {
    int other_id = entry.first;
    rclcpp::Time last_seen = entry.second;

    // Check if the node is alive before yielding
    // If it's dead, we ignore it (it can't be leader)
    if ((now - last_seen).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * 2) {
      continue;
    }

    if (other_id > id_) {
      // Found a higher ID that is ALIVE, do not become leader
      return;
    }
  }
  std_msgs::msg::Int32 msg;
  msg.data = id_;
  election_pub_->publish(msg);
  // RCLCPP_INFO(get_logger(), "Agent %d broadcasting leadership", id_);
}

void SimpleAgent::run_health_check()
{
  rclcpp::Time now = this->now();

  // Check leader heartbeat
  if (leader_id_ != id_){
    if (last_heartbeat_map_.find(leader_id_) == last_heartbeat_map_.end()) {
      // Never received heartbeat from leader
      RCLCPP_WARN(get_logger(), "Agent %d has never received heartbeat from leader Agent %d", id_, leader_id_);
      run_election_logic(); 
      return;
    }else if ((now - last_heartbeat_map_[leader_id_]).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * 2) {
      RCLCPP_WARN(get_logger(), "Agent %d detected failure of leader Agent %d", id_, leader_id_);
      run_election_logic(); 
      return;
    }
  }else{
    for (const auto & agent : last_heartbeat_map_) {
      if (agent.first == id_) continue;
      if ((now - agent.second).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * 2) {
        RCLCPP_WARN(get_logger(), "Leader %d detected failure of Agent %d", id_, agent.first);
        // revive dead node
        std_msgs::msg::Int32 msg;
        msg.data = agent.first;
        revival_pub_->publish(msg);
      }
    }
  }
}

}  // namespace distributed_election
