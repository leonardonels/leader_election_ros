#include "distributed_election/simple_agent.hpp"
#include "std_msgs/msg/int32_multi_array.hpp"

namespace distributed_election
{
class RingAgent : public SimpleAgent
{
public:
  RingAgent(const std::string & node_name, int id, int heartbeat_interval_ms, int heartbeat_max_tick);
  
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

  // Ping mechanism for leader to verify node status
  rclcpp::Publisher<std_msgs::msg::Int32>::SharedPtr ping_pub_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr ping_sub_;
  void on_ping_received(const std_msgs::msg::Int32::SharedPtr msg);

private:
  void publish_heartbeat() override;
  void on_heartbeat() override;
  
  // Watchdog
  int monitored_successor_;

  // Election Watchdog
  int monitored_election_successor_;
  
  // Counter-based liveness  
  int last_token_tick_, current_tick_, heartbeat_max_tick_;
  std::map<int, int> last_heartbeat_tick_map_;

  // Leader tracks when nodes were last pinged
  std::map<int, int> last_ping_tick_;

  // Startup timer
  bool election_ready_;
  rclcpp::TimerBase::SharedPtr startup_timer_;
  void on_startup_timer();

};
}