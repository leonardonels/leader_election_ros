#include "distributed_election/traffic_logger.hpp"

#include <sstream>

namespace distributed_election
{

TrafficLogger::TrafficLogger(const std::string & agent_type)
: Node("traffic_logger"), start_time_(this->now())
{
  RCLCPP_INFO(this->get_logger(), "Starting Traffic Logger for Agent Type: %s", agent_type.c_str());

  // Use a user-directory or a safe path
  std::string filename = "distributed_election_metrics.csv";
  csv_file_.open(filename, std::ios::out | std::ios::trunc);
  if (csv_file_.is_open()) {
      csv_file_ << "Timestamp,EventType,NodeID,Data" << std::endl;
      RCLCPP_INFO(this->get_logger(), "Logging metrics to %s", filename.c_str());
  } else {
      RCLCPP_ERROR(this->get_logger(), "Failed to open log file: %s", filename.c_str());
  }

  // Periodic analysis timer (every 5 seconds)
  analysis_timer_ = this->create_wall_timer(
    std::chrono::seconds(30),
    std::bind(&TrafficLogger::periodic_analysis, this));

  rclcpp::QoS qos_profile(20);
  qos_profile.best_effort();

  // Common subscriptions
  heartbeat_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
    "/election/heartbeats", 
    qos_profile, 
    std::bind(&TrafficLogger::log_heartbeat, this, std::placeholders::_1));

  leader_sub_ = this->create_subscription<std_msgs::msg::Int32>(
    "/election/leader", 
    qos_profile, 
    std::bind(&TrafficLogger::log_leader, this, std::placeholders::_1));

  revive_sub_ = this->create_subscription<std_msgs::msg::Int32>(
    "/election/revive", 
    qos_profile, 
    std::bind(&TrafficLogger::log_revive, this, std::placeholders::_1));

  // Type specific subscriptions
  if (agent_type == "ring" || agent_type == "hybrid_ring") {
    ring_token_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
      "/election/ring_token", 
      qos_profile, 
      std::bind(&TrafficLogger::log_ring_token, this, std::placeholders::_1));
    
    map_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
      "/election/map", 
      qos_profile, 
      std::bind(&TrafficLogger::log_map, this, std::placeholders::_1));
  }
  else if (agent_type == "bully") {
      map_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
      "/election/map", 
      qos_profile, 
      std::bind(&TrafficLogger::log_map, this, std::placeholders::_1));
  }
  else if (agent_type == "raft") {
    vote_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
      "/election/vote", 
      qos_profile, 
      std::bind(&TrafficLogger::log_vote, this, std::placeholders::_1));
  }
  // benevolent_dictator uses only common topics
}

TrafficLogger::~TrafficLogger()
{
  if (csv_file_.is_open()) {
    csv_file_.close();
  }
}

void TrafficLogger::log_to_file(const std::string & type, const std::string & data)
{
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << "," << type << ",," << data << "\n"; // No generic NodeID for system events
  }
}

void TrafficLogger::update_node_stats(int node_id)
{
  rclcpp::Time now = this->now();
  if (node_stats_.find(node_id) == node_stats_.end()) {
    node_stats_[node_id].first_seen = now;
    node_stats_[node_id].is_alive = true;
    node_stats_[node_id].death_time = rclcpp::Time(0, 0, this->get_clock()->get_clock_type());
  }
  
  if (!node_stats_[node_id].is_alive) {
      // Node was dead, now back alive (REVIVED!)
      node_stats_[node_id].is_alive = true;
      
      if (node_stats_[node_id].death_time.nanoseconds() > 0) {
          double downtime = (now - node_stats_[node_id].death_time).seconds();
          
          if (csv_file_.is_open()) {
             double timestamp = now.seconds();
             csv_file_ << timestamp << ",REVIVAL_TIME," << node_id << "," << downtime << "\n";
          }
          RCLCPP_INFO(this->get_logger(), ">> NODE %d REVIVED! Downtime: %.2fs <<", node_id, downtime);
      }
      node_stats_[node_id].death_time = rclcpp::Time(0, 0, this->get_clock()->get_clock_type()); // Reset
  }

  node_stats_[node_id].last_seen = now;
  node_stats_[node_id].msg_count++;
  
  // Simple uptime calculation: difference between now and first seen
  // This is technically "Lifetime". "Uptime" would require detecting deaths.
  // Given chaos monkey kills are not directly signaled to us (unless we sniff them?),
  // we can only estimate based on last_heartbeat.
  node_stats_[node_id].total_uptime_sec = (now - node_stats_[node_id].first_seen).seconds();
}

void TrafficLogger::periodic_analysis()
{
  rclcpp::Time now = this->now();
  double elapsed = (now - start_time_).seconds();
  double rate = total_messages_ / (elapsed > 0 ? elapsed : 1.0);

  // Check for dead nodes
  int alive_nodes = 0;
  for (auto & pair : node_stats_) {
    double time_since_last = (now - pair.second.last_seen).seconds();
    if (time_since_last < 3.0) { // 3 seconds timeout threshold for stats
       alive_nodes++;
       // We considered it alive.
    } else {
       // CONSIDERED DEAD based on traffic
       if (pair.second.is_alive) {
           // Transition from Alive -> Dead
           pair.second.is_alive = false;
           // We use the time of "detection" as death time, although the actual death was earlier.
           // Or we could use last_seen + threshold. Let's use 'now' as the detection time.
           pair.second.death_time = now;
           RCLCPP_WARN(this->get_logger(), "Node %d considered DEAD (last seen %.1fs ago)", 
              pair.first, time_since_last);
       }
    }
  }

  std::stringstream ss;
  ss << "Traffic Analysis [Elapsed: " << elapsed << "s] | ";
  ss << "Msgs: " << total_messages_ << " (" << rate << " msg/s) | ";
  ss << "Alive Nodes: " << alive_nodes;
  
  RCLCPP_INFO(this->get_logger(), "%s", ss.str().c_str());
  
  // Detailed stats could be logged to file or verbose
}

void TrafficLogger::log_heartbeat(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.empty()) return;
  int sender_id = msg->data[0];
  
  update_node_stats(sender_id);
  total_messages_++;

  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    // Format: Timestamp,EventType,NodeID,Data
    csv_file_ << timestamp << ",HEARTBEAT," << sender_id << "," << array_to_string(msg->data) << "\n";
  }
}

void TrafficLogger::log_leader(const std_msgs::msg::Int32::SharedPtr msg)
{
  total_messages_++;
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << ",LEADER,," << msg->data << "\n";
  }
  RCLCPP_INFO(this->get_logger(), ">> LEADER CHANGE DETECTED: Node %d <<", msg->data);
}

void TrafficLogger::log_revive(const std_msgs::msg::Int32::SharedPtr msg)
{
  total_messages_++;
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << ",REVIVE,," << msg->data << "\n";
  }
  // Keep important events visible
  // RCLCPP_INFO(this->get_logger(), "[REVIVE] Target: %d", msg->data);
}

void TrafficLogger::log_ring_token(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  total_messages_++;
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << ",RING_TOKEN,," << array_to_string(msg->data) << "\n";
  }
}

void TrafficLogger::log_map(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  total_messages_++;
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << ",MAP,," << array_to_string(msg->data) << "\n";
  }
}

void TrafficLogger::log_vote(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  total_messages_++;
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << ",VOTE,," << array_to_string(msg->data) << "\n";
  }
}

std::string TrafficLogger::array_to_string(const std::vector<int> & data)
{
  std::stringstream ss;
  ss << "[";
  for (size_t i = 0; i < data.size(); ++i) {
    ss << data[i];
    if (i < data.size() - 1) {
      ss << ", ";
    }
  }
  ss << "]";
  return ss.str();
}

} // namespace distributed_election

/* 
// Main function moved to separate file or removed if not needed standalone 
int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  // This fails now because constructor requires args
  // rclcpp::spin(std::make_shared<distributed_election::TrafficLogger>());
  rclcpp::shutdown();
  return 0;
}
*/
