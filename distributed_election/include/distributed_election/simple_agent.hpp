#ifndef DISTRIBUTED_ELECTION__SIMPLE_AGENT_HPP_
#define DISTRIBUTED_ELECTION__SIMPLE_AGENT_HPP_

#include <memory>
#include <string>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "std_msgs/msg/int32.hpp"

namespace distributed_election
{

class SimpleAgent : public rclcpp_lifecycle::LifecycleNode
{
public:
  explicit SimpleAgent(const std::string & node_name, int id, int heartbeat_interval_ms);

  using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

  CallbackReturn on_configure(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_activate(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_deactivate(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_cleanup(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_shutdown(const rclcpp_lifecycle::State &) override;

private:
  void publish_heartbeat();

  int id_;
  int heartbeat_interval_ms_;
  std::shared_ptr<rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::Int32>> heartbeat_pub_;
  rclcpp::TimerBase::SharedPtr timer_;
};

}  // namespace distributed_election

#endif  // DISTRIBUTED_ELECTION__SIMPLE_AGENT_HPP_
