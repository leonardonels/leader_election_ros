#include "distributed_election/traffic_logger.hpp"

#include <sstream>
#include <iomanip>
#include <iostream>

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

  analysis_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(100),
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
  std::lock_guard<std::mutex> lock(mutex_);
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << "," << type << ",," << data << "\n"; // No generic NodeID for system events
  }
}

void TrafficLogger::update_node_stats(int node_id)
{
  // Expects mutex to be held by caller since it's a private helper usually called from locked context
  // BUT: log_heartbeat calls this. If I lock in log_heartbeat, I don't need to lock here.
  // HOWEVER: update_node_stats is NOT called in log_leader etc.
  // So: I will lock in the public/callback methods.
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
          // Because periodic_analysis runs frequently and death_time is set to 'now' at detection,
          // the "downtime" is (now - detection_time). 
          // But real death was at (detection_time - timeout). 
          // And real revival is 'now'.
          // So downtime = (now - death_time) + 3.0 (timeout threshold)
          // This gives a much closer estimate to reality.
          double downtime = (now - node_stats_[node_id].death_time).seconds() + 3.0; // Add the detection lag
          
          node_stats_[node_id].accumulated_downtime_sec += downtime;
          // node_stats_[node_id].failure_count++; // Already incremented on detection

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
  std::lock_guard<std::mutex> lock(mutex_);
  rclcpp::Time now = this->now();
  double elapsed = (now - start_time_).seconds();
  double rate = total_messages_ / (elapsed > 0 ? elapsed : 1.0);

  // Check for dead nodes
  int alive_nodes = 0;
  for (auto & pair : node_stats_) {
    double time_since_last = (now - pair.second.last_seen).seconds();
    if (time_since_last < 3.0) { // 3 seconds timeout threshold for stats
       alive_nodes++;
    } else {
       // CONSIDERED DEAD based on traffic
       if (pair.second.is_alive) {
           pair.second.is_alive = false;
           pair.second.death_time = now;
           pair.second.failure_count++; // Increment failure count immediately on detection
       }
    }
  }

  // reduce log spam by printing every 30 seconds only
  if ( ((int)elapsed) % 30 != 0 || ((int)elapsed) == 0 || elapsed - (int)elapsed > 0.1 ) {
      return;
  }

  std::stringstream ss;
  ss << "\n=== Traffic Analysis [T+" << (int)elapsed << "s] ===\n";
  ss << "Msgs: " << total_messages_ << " (" << std::fixed << std::setprecision(1) << rate << " msg/s) | Alive: " << alive_nodes << "\n";
  ss << "Leader Changes: " << leader_change_count_ << " | Current Leader: " << (current_leader_id_ == -1 ? "None" : std::to_string(current_leader_id_)) << "\n";
  ss << " ID | Status        | Uptime(s) | Failures | MTTR (s)\n";
  ss << "----|---------------|-----------|----------|---------\n";
  
  for (const auto & pair : node_stats_) {
      int id = pair.first;
      const auto & stats = pair.second;
      
      std::string status_str;
      if (stats.is_alive) {
          status_str = "ALIVE";
      } else {
          double current_down = (now - stats.death_time).seconds() + 3.0; // Add detection lag
          std::stringstream tmp;
          tmp << "DEAD (" << (int)current_down << "s)";
          status_str = tmp.str();
      }
      
      double avg_downtime = 0.0;
      int completed_failures = stats.failure_count;
      if (!stats.is_alive) completed_failures--; // Don't count current unfinished failure for average

      if (completed_failures > 0) {
          avg_downtime = stats.accumulated_downtime_sec / completed_failures;
      }
      
      ss << " " << std::setw(2) << id << " | " 
         << std::left << std::setw(13) << status_str << std::right << " | " 
         << std::setw(9) << (int)stats.total_uptime_sec << " | " 
         << std::setw(8) << stats.failure_count << " | " 
         << std::fixed << std::setprecision(2) << avg_downtime << "\n";
  }
  
  // Use std::cout directly to avoid log prefixes messing up the table alignment
  std::cout << ss.str() << std::flush;
}

void TrafficLogger::log_heartbeat(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.empty()) return;

  std::lock_guard<std::mutex> lock(mutex_);
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
  std::lock_guard<std::mutex> lock(mutex_);
  total_messages_++;
  
  if (msg->data != current_leader_id_) {
      leader_change_count_++;
      current_leader_id_ = msg->data;
      RCLCPP_INFO(this->get_logger(), ">> LEADER CHANGE DETECTED: Node %d (Total Changes: %d) <<", msg->data, leader_change_count_);
  }

  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << ",LEADER,," << msg->data << "\n";
  }
}

void TrafficLogger::log_revive(const std_msgs::msg::Int32::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
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
  std::lock_guard<std::mutex> lock(mutex_);
  total_messages_++;
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << ",RING_TOKEN,," << array_to_string(msg->data) << "\n";
  }
}

void TrafficLogger::log_map(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
  total_messages_++;
  if (csv_file_.is_open()) {
    double timestamp = this->now().seconds();
    csv_file_ << timestamp << ",MAP,," << array_to_string(msg->data) << "\n";
  }
}

void TrafficLogger::log_vote(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(mutex_);
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
