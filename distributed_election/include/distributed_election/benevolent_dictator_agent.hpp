#ifndef DISTRIBUTED_ELECTION__BENEVOLENT_DICTATOR_AGENT_HPP_
#define DISTRIBUTED_ELECTION__BENEVOLENT_DICTATOR_AGENT_HPP_

#include "distributed_election/simple_agent.hpp"

namespace distributed_election
{

class BenevolentDictatorAgent : public SimpleAgent
{
public:
  // Acronym: BEDA (Benevolent Dictator Agent)
  explicit BenevolentDictatorAgent(const std::string & node_name, int id, int heartbeat_interval_ms, int heartbeat_max_tick = 2);

  CallbackReturn on_configure(const rclcpp_lifecycle::State & state) override;
  CallbackReturn on_cleanup(const rclcpp_lifecycle::State & state) override;

protected:
  void on_heartbeat() override;
  void on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg) override;
  void on_revive_received(const std_msgs::msg::Int32::SharedPtr msg);
  void on_watchdog_timeout();
  
  // Flag to track if a revive for the leader (Node 0) has specificly been seen
  // which implies we should wait for the leader to wake up.
  bool leader_revive_sent_;

  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr revive_sub_;
  rclcpp::TimerBase::SharedPtr watchdog_timer_;
  rclcpp::TimerBase::SharedPtr revival_delay_timer_;
};

}  // namespace distributed_election

#endif  // DISTRIBUTED_ELECTION__BENEVOLENT_DICTATOR_AGENT_HPP_
