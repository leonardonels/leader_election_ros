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

  // Create a timer to run the election logic periodically (e.g., every 500ms)
  election_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(500),
    std::bind(&SimpleAgent::run_election_logic, this));

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

void SimpleAgent::run_election_logic()
{
  rclcpp::Time current_time = this->now();
  std::vector<int> to_remove;

  for (const auto & entry : last_heartbeat_map_) {
    int agent_id = entry.first;
    rclcpp::Time last_seen = entry.second;

    // If we haven't seen a heartbeat from this agent in twice the heartbeat interval, consider it dead
    if ((current_time - last_seen).nanoseconds() / 1e6 > 2 * heartbeat_interval_ms_) {
      RCLCPP_WARN(get_logger(), "Agent %d considered dead (last seen at %f)", agent_id, last_seen.seconds());
      to_remove.push_back(agent_id);
    }
  }

  // Remove dead agents from the map
  for (int agent_id : to_remove) {
    last_heartbeat_map_.erase(agent_id);
  }
}

}  // namespace distributed_election
