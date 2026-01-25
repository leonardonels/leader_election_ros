#include "distributed_election_simulation/simple_agent.hpp"
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
  
  int get_successor();
  void forward_heartbeat(std_msgs::msg::Int32MultiArray msg);
  void forward_token(std_msgs::msg::Int32MultiArray msg);
  void publish_heartbeat() override;
  void share_map();

  void on_startup_timer();
  
  void on_heartbeat() override;
  void on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg) override;
  
  void run_election_logic() override;
  void on_token_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  void on_leader_received(const std_msgs::msg::Int32::SharedPtr msg) override;
  void on_map_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg);

  // Ring token pub and sub
  rclcpp::Publisher<std_msgs::msg::Int32MultiArray>::SharedPtr token_pub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr token_sub_;
  
  // Map sharing pub and sub
  rclcpp::Publisher<std_msgs::msg::Int32MultiArray>::SharedPtr map_pub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr map_sub_;

  // Counter-based liveness  
  int last_token_tick_, current_tick_;
  std::map<int, int> last_heartbeat_tick_map_;
  std::map<int, int> network_view_map_;

  // Startup timer
  bool election_ready_;
  rclcpp::TimerBase::SharedPtr startup_timer_;

};
}