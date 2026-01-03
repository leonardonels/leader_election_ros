#include "distributed_election/simple_agent.hpp"
#include "std_msgs/msg/int32_multi_array.hpp"

namespace distributed_election
{
class RingAgent : public SimpleAgent
{
public:
  RingAgent(const std::string & node_name, int id, int heartbeat_interval_ms);
  
  using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;
  CallbackReturn on_configure(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_activate(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_deactivate(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_cleanup(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_shutdown(const rclcpp_lifecycle::State &) override;

protected:
  void on_leader_received(const std_msgs::msg::Int32::SharedPtr msg) override;
  void run_election_logic() override;
  void on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg) override;
  
  void on_token_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  int get_successor();

  // Ring token pub and sub
  rclcpp::Publisher<std_msgs::msg::Int32MultiArray>::SharedPtr token_pub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr token_sub_;

  // Election state for aggregation
  std::set<int> known_candidates_;

private:
  void publish_heartbeat() override;
  void run_health_check() override;
  
  // Watchdog
  rclcpp::TimerBase::SharedPtr watchdog_timer_;
  void on_watchdog_timeout();
  std_msgs::msg::Int32MultiArray pending_token_;
  int monitored_successor_;
  rclcpp::Time last_token_;

  // Startup delay
  bool election_ready_;
  rclcpp::TimerBase::SharedPtr startup_timer_;
  void on_startup_timer();

  // Easy timers
  int startup_time;
  int health_time;
  int watchdog_time;
  int timeout_time;
};
}