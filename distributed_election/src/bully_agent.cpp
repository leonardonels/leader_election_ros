#include "distributed_election/bully_agent.hpp"

namespace distributed_election
{

BullyAgent::BullyAgent(const std::string & node_name, int id, int heartbeat_interval_ms)
: SimpleAgent(node_name, id, heartbeat_interval_ms)
{
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

void BullyAgent::run_election_logic()
{  
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
