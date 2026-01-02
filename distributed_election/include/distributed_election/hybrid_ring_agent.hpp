#include "distributed_election/simple_agent.hpp"
#include "std_msgs/msg/int32_multi_array.hpp"

namespace distributed_election
{
class HybridRingAgent : public SimpleAgent
{
public:
  HybridRingAgent(const std::string & node_name, int id, int heartbeat_interval_ms);
  
  using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;
  CallbackReturn on_configure(const rclcpp_lifecycle::State &) override;

protected:
  void on_leader_received(const std_msgs::msg::Int32::SharedPtr msg) override;
  void run_election_logic() override;
  
  void on_token_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  int get_successor();
  void on_map_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  void gossip_map();

  // Ring token pub and sub
  rclcpp::Publisher<std_msgs::msg::Int32MultiArray>::SharedPtr token_pub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr token_sub_;

  // Network map pub and sub
  rclcpp::Publisher<std_msgs::msg::Int32MultiArray>::SharedPtr map_pub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr map_sub_;
  rclcpp::TimerBase::SharedPtr gossip_timer_;
  
  // Election state for aggregation
  std::set<int> known_candidates_;
  
  // Startup delay
  bool election_ready_;
  rclcpp::TimerBase::SharedPtr startup_timer_;
  void on_startup_timer();
};
}