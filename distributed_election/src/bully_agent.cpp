#include "distributed_election/bully_agent.hpp"

namespace distributed_election
{

BullyAgent::BullyAgent(const std::string & node_name, int id, int heartbeat_interval_ms)
: SimpleAgent(node_name, id, heartbeat_interval_ms),
  election_ready_(false)
{
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
BullyAgent::on_configure(const rclcpp_lifecycle::State & state)
{
  // ----------------------------------- Simple Agent Setup -----------------------------------
  auto result = SimpleAgent::on_configure(state);
  if (result != rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS) {
    return result;
  }

  // ----------------------------------- Bully Agent Setup -----------------------------------
  rclcpp::QoS qos_profile(1);
  qos_profile.best_effort();

  map_pub_ = this->create_publisher<std_msgs::msg::Int32MultiArray>("/election/map", qos_profile);

  map_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
    "/election/map", 
    qos_profile, 
    std::bind(&BullyAgent::on_map_received, this, std::placeholders::_1));

  gossip_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 5),
    std::bind(&BullyAgent::gossip_map, this));

  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * 2),
    std::bind(&BullyAgent::on_startup_timer, this));

  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

void BullyAgent::gossip_map()
{
  if (leader_id_ == id_) {
    return;
  }
  std_msgs::msg::Int32MultiArray msg;
  msg.data.clear();
  for (const auto & entry : last_heartbeat_map_) {
    msg.data.push_back(entry.first);
  }
  map_pub_->publish(msg);
}

void BullyAgent::on_map_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (leader_id_ != id_) {
    return;
  }
  for (const auto & agent_id : msg->data) {
    if (last_heartbeat_map_.find(agent_id) == last_heartbeat_map_.end()) {
      RCLCPP_INFO(get_logger(), "Leader %d adding Agent %d to network map", id_, agent_id);
      last_heartbeat_map_[agent_id] = this->now();
    }
  }
}

void BullyAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  leader_id_ = msg->data;
  if (leader_id_ >= id_){
    RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);
  }else{
    RCLCPP_WARN(get_logger(), "Agent %d received leader %d with lower ID! Taking over...", id_, leader_id_);
    leader_id_ = id_;
    std_msgs::msg::Int32 msg;
    msg.data = id_;
    election_pub_->publish(msg);
  }
}

// Small semplification to avoid elections every dead node startup
void BullyAgent::on_startup_timer()
{
  startup_timer_->cancel();
  election_ready_ = true;
  // RCLCPP_INFO(get_logger(), "Startup delay finished. Starting election logic.");
  leader_id_ = id_;
  for (const auto & entry : last_heartbeat_map_) {
    int other_id = entry.first;
    if (other_id > id_ && (this->now() - entry.second).nanoseconds() * 1e-6 <= heartbeat_interval_ms_ * 2) {
      leader_id_ = other_id;
    }
  }
  if (leader_id_ == id_) {
    run_election_logic();
  }else{
    RCLCPP_INFO(get_logger(), "Agent %d recognizes existing leader: Agent %d", id_, leader_id_);
  }
}

void BullyAgent::run_election_logic()
{  
  if (!election_ready_) {
    return;
  }
  rclcpp::Time now = this->now();
  // test logic: broadcast bully
  for (const auto & entry : last_heartbeat_map_) {
    int other_id = entry.first;
    rclcpp::Time last_seen = entry.second;

    // Check if the node is alive before yielding
    // If it's dead, we ignore it (it can't be leader)
    if ((now - last_seen).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * 2) {
      continue;
    }

    if (other_id > id_) {
      // Found a higher ID that is ALIVE, do not become leader
      return;
    }
  }
  std_msgs::msg::Int32 msg;
  msg.data = id_;
  election_pub_->publish(msg);
  // RCLCPP_INFO(get_logger(), "Agent %d broadcasting leadership", id_);
}

}  // namespace distributed_election
