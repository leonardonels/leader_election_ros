#ifndef DISTRIBUTED_ELECTION__TRAFFIC_LOGGER_HPP_
#define DISTRIBUTED_ELECTION__TRAFFIC_LOGGER_HPP_

#include <memory>
#include <string>
#include <map>
#include <fstream>
#include <chrono>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/int32.hpp"
#include "std_msgs/msg/int32_multi_array.hpp"

namespace distributed_election
{

struct NodeStats {
    rclcpp::Time first_seen;
    rclcpp::Time last_seen;
    rclcpp::Time death_time; // Time when we considered it dead
    double total_uptime_sec = 0.0;
    int msg_count = 0;
    bool is_alive = false;
};

class TrafficLogger : public rclcpp::Node
{
public:
  explicit TrafficLogger(const std::string & agent_type);
  virtual ~TrafficLogger();

private:
  void log_heartbeat(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  void log_leader(const std_msgs::msg::Int32::SharedPtr msg);
  void log_revive(const std_msgs::msg::Int32::SharedPtr msg);
  void log_ring_token(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  void log_map(const std_msgs::msg::Int32MultiArray::SharedPtr msg);
  void log_vote(const std_msgs::msg::Int32MultiArray::SharedPtr msg);

  void update_node_stats(int node_id);
  void periodic_analysis();
  void log_to_file(const std::string & type, const std::string & data);

  // Helper to parse Int32MultiArray to string for logging
  std::string array_to_string(const std::vector<int> & data);

  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr heartbeat_sub_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr leader_sub_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr revive_sub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr ring_token_sub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr map_sub_;
  rclcpp::Subscription<std_msgs::msg::Int32MultiArray>::SharedPtr vote_sub_;

  // Metrics
  std::ofstream csv_file_;
  std::map<int, NodeStats> node_stats_;
  rclcpp::TimerBase::SharedPtr analysis_timer_;
  rclcpp::Time start_time_;
  
  // Track total traffic
  long long total_messages_ = 0;
};

}  // namespace distributed_election

#endif  // DISTRIBUTED_ELECTION__TRAFFIC_LOGGER_HPP_
