#ifndef DISTRIBUTED_ELECTION__SIMPLE_AGENT_HPP_
#define DISTRIBUTED_ELECTION__SIMPLE_AGENT_HPP_

#include <memory>
#include <string>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "std_msgs/msg/int32.hpp"
#include "std_msgs/msg/int32_multi_array.hpp"

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
  
  protected:
  virtual void on_leader_received(const std_msgs::msg::Int32::SharedPtr msg);
  virtual void run_election_logic();
  virtual void publish_heartbeat();
  virtual void on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  virtual void run_health_check();
  
  int id_;
  int32_t leader_id_;
  int heartbeat_interval_ms_;
  std::shared_ptr<rclcpp_lifecycle::LifecyclePublisher<std_msgs::msg::Int32MultiArray>> heartbeat_pub_;
  rclcpp::TimerBase::SharedPtr timer_;

  // Map of Agent ID -> Last time seen
  std::map<int, rclcpp::Time> last_heartbeat_map_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr heartbeat_sub_;
  rclcpp::TimerBase::SharedPtr health_check_timer_;

  // Leader election pub and sub
  rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr election_pub_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr election_sub_;
  
  // Revival mechanism
  rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr revival_pub_;

};

}  // namespace distributed_election

#endif  // DISTRIBUTED_ELECTION__SIMPLE_AGENT_HPP_
