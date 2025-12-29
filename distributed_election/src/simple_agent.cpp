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
  std::string topic_name = "/heartbeat/agent_" + std::to_string(id_);
  heartbeat_pub_ = this->create_publisher<std_msgs::msg::Int32>(topic_name, 10);
  
  timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_),
    std::bind(&SimpleAgent::publish_heartbeat, this));

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_activate(const rclcpp_lifecycle::State &)
{
  RCLCPP_INFO(get_logger(), "Activating agent %d", id_);
  heartbeat_pub_->on_activate();
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_deactivate(const rclcpp_lifecycle::State &)
{
  RCLCPP_INFO(get_logger(), "Deactivating agent %d", id_);
  heartbeat_pub_->on_deactivate();
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_cleanup(const rclcpp_lifecycle::State &)
{
  RCLCPP_INFO(get_logger(), "Cleaning up agent %d", id_);
  heartbeat_pub_.reset();
  timer_.reset();
  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
SimpleAgent::on_shutdown(const rclcpp_lifecycle::State &)
{
  RCLCPP_INFO(get_logger(), "Shutting down agent %d", id_);
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
    RCLCPP_DEBUG(get_logger(), "Agent %d sent heartbeat", id_);
  }
}

}  // namespace distributed_election
