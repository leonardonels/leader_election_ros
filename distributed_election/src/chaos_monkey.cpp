#include "distributed_election/chaos_monkey.hpp"

using namespace std::chrono_literals;

namespace distributed_election
{

ChaosMonkey::ChaosMonkey(
    const std::string & target_prefix, 
    int kill_interval_s, 
    int discovery_time_s, 
    bool reverse_order)
: Node("chaos_monkey"), 
  target_prefix_(target_prefix),
  kill_interval_s_(kill_interval_s),
  reverse_order_(reverse_order)
{
  RCLCPP_INFO(this->get_logger(), "Chaos Monkey Configured with Prefix: '%s', Interval: %ds", target_prefix_.c_str(), kill_interval_s_);

  rng_.seed(std::random_device()());

  if (discovery_time_s > 0) 
  {
    RCLCPP_INFO(this->get_logger(), "Chaos Monkey waiting %d seconds before starting", discovery_time_s);
    discovery_timer_ = this->create_wall_timer(
      std::chrono::seconds(discovery_time_s),
      [this]() {
        RCLCPP_INFO(this->get_logger(), "Chaos Monkey starting node failures");
        // Start the actual kill timer
        this->timer_ = this->create_wall_timer(
            std::chrono::seconds(kill_interval_s_),
            std::bind(&ChaosMonkey::kill_random_node, this));
        
        // Cancel self
        if (this->discovery_timer_) {
            this->discovery_timer_->cancel();
            this->discovery_timer_.reset();
        }
      });
  } else {
      // Start immediately
      timer_ = this->create_wall_timer(
        std::chrono::seconds(kill_interval_s_),
        std::bind(&ChaosMonkey::kill_random_node, this));
  }
}

void ChaosMonkey::kill_random_node()
{
  auto node_names = this->get_node_graph_interface()->get_node_names();
  std::vector<std::string> target_candidates;

  for (const auto & name : node_names) {
    if (name.find(target_prefix_) != std::string::npos) {
      target_candidates.push_back(name);
    }
  }

  if (target_candidates.empty()) {
    RCLCPP_WARN(get_logger(), "No nodes found matching prefix '%s'", target_prefix_.c_str());
    return;
  }
  
  if (reverse_order_) {
    std::sort(target_candidates.rbegin(), target_candidates.rend());
  } else {
    std::sort(target_candidates.begin(), target_candidates.end());
  }

  // Weighted random selection favouring higher indices
  std::vector<double> weights;
  for (size_t i = 0; i < target_candidates.size(); ++i) {
      weights.push_back(static_cast<double>(i + 1));
  }

  std::discrete_distribution<int> dist(weights.begin(), weights.end());
  int index = dist(rng_);
  std::string node_name = target_candidates[index];

  RCLCPP_INFO(get_logger(), " ");
  RCLCPP_INFO(get_logger(), "Chaos Monkey now targeting: %s", node_name.c_str());
  
  std::string get_state_service_name = node_name + "/get_state";
  std::string change_state_service_name = node_name + "/change_state";

  auto get_state_client = this->create_client<lifecycle_msgs::srv::GetState>(get_state_service_name);
  auto change_state_client = this->create_client<lifecycle_msgs::srv::ChangeState>(change_state_service_name);
  
  // Checking availability with a timeout. If not available, skip this cycle.
  // Note: Blocking in a timer callback for too long is bad, but 1s is manageable here.
  // Ideally we would do this purely async, but for simplicity:
  if (!get_state_client->wait_for_service(1s) || !change_state_client->wait_for_service(1s)) {
    RCLCPP_WARN(get_logger(), "Services for %s not available. Skipping.", node_name.c_str());
    return;
  }

  auto request = std::make_shared<lifecycle_msgs::srv::GetState::Request>();
  
  using GetStateClient = rclcpp::Client<lifecycle_msgs::srv::GetState>;
  using ChangeStateClient = rclcpp::Client<lifecycle_msgs::srv::ChangeState>;

  get_state_client->async_send_request(request, 
    [this, node_name, get_state_client, change_state_client](GetStateClient::SharedFuture future_state) {
      try {
        auto state_resp = future_state.get();
        uint8_t current_state = state_resp->current_state.id;
        
        uint8_t transition_id = 0;
        std::string log_label;

        if (current_state == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE) {
            transition_id = lifecycle_msgs::msg::Transition::TRANSITION_ACTIVE_SHUTDOWN;
            log_label = "ACTIVE_SHUTDOWN";
        } else if (current_state == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE) {
            transition_id = lifecycle_msgs::msg::Transition::TRANSITION_INACTIVE_SHUTDOWN;
            log_label = "INACTIVE_SHUTDOWN";
        } else if (current_state == lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED) {
            transition_id = lifecycle_msgs::msg::Transition::TRANSITION_UNCONFIGURED_SHUTDOWN;
            log_label = "UNCONFIGURED_SHUTDOWN";
        } else {
            // Already dead or finalizing
            return;
        }

        auto change_req = std::make_shared<lifecycle_msgs::srv::ChangeState::Request>();
        change_req->transition.id = transition_id;

        RCLCPP_INFO(this->get_logger(), "Sending %s (id %d) to %s", log_label.c_str(), transition_id, node_name.c_str());

        change_state_client->async_send_request(change_req, 
          [this, node_name, change_state_client](ChangeStateClient::SharedFuture future_change) {
            try {
              if (future_change.get()->success) {
                  RCLCPP_INFO(this->get_logger(), "Successfully shut down %s", node_name.c_str());
              } else {
                  RCLCPP_WARN(this->get_logger(), "Failed to shut down %s", node_name.c_str());
              }
            } catch (...) {
               RCLCPP_ERROR(this->get_logger(), "Change state service call failed for %s", node_name.c_str());
            }
          });
      } catch (...) {
        RCLCPP_ERROR(this->get_logger(), "Get state service call failed for %s", node_name.c_str());
      }
    });
}

} // namespace distributed_election
