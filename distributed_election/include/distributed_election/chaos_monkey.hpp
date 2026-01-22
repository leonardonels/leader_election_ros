#ifndef DISTRIBUTED_ELECTION__CHAOS_MONKEY_HPP_
#define DISTRIBUTED_ELECTION__CHAOS_MONKEY_HPP_

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <random>

#include "rclcpp/rclcpp.hpp"
#include "lifecycle_msgs/msg/state.hpp"
#include "lifecycle_msgs/msg/transition.hpp"
#include "lifecycle_msgs/srv/change_state.hpp"
#include "lifecycle_msgs/srv/get_state.hpp"

namespace distributed_election
{

class ChaosMonkey : public rclcpp::Node
{
public:
  ChaosMonkey(
    const std::string & target_prefix, 
    int kill_interval_s, 
    int discovery_time_s, 
    bool reverse_order,
    bool reduce_race_conditions);

  virtual ~ChaosMonkey() = default;

private:
  void kill_random_node();

  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::TimerBase::SharedPtr discovery_timer_;
  std::mt19937 rng_;

  std::string target_prefix_;
  int kill_interval_s_;
  bool reverse_order_;
  bool reduce_race_conditions_;

  int last_killed_id_;
};

} // namespace distributed_election

#endif // DISTRIBUTED_ELECTION__CHAOS_MONKEY_HPP_
