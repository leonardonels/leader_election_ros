#include "distributed_election/simple_agent.hpp"
#include "std_msgs/msg/int32_multi_array.hpp"

namespace distributed_election
{
class RaftAgent : public SimpleAgent
{
public:
  RaftAgent(const std::string & node_name, int id, int heartbeat_interval_ms, int heartbeat_max_tick);
  
  using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;
  CallbackReturn on_configure(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_activate(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_deactivate(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_cleanup(const rclcpp_lifecycle::State &) override;
  CallbackReturn on_shutdown(const rclcpp_lifecycle::State &) override;

protected:
  int choose_leader();

  void on_startup_timer();
  void on_heartbeat() override;
  void on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg) override;
  
  void run_election_logic() override;
  void on_vote_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  void on_leader_received(const std_msgs::msg::Int32::SharedPtr msg) override;
  void on_watchdog_timeout();

  // Election vote management
  rclcpp::Publisher<std_msgs::msg::Int32MultiArray>::SharedPtr vote_pub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr vote_sub_;
  rclcpp::TimerBase::SharedPtr election_watchdog_timer_;
  std::map<int, int> election_map_;
  int initiator_id_;
  bool is_election_in_progress_;
  bool was_i_the_initiator_;

  // Counter-based liveness  
  std::map<int, int> last_heartbeat_tick_map_;

  // Suspected dead agents map (for leader to track revivals)
  std::map<int, rclcpp::Time> suspected_dead_agents_;

  // Startup timer
  rclcpp::TimerBase::SharedPtr startup_timer_;

};
}