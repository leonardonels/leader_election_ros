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

using namespace std::chrono_literals;

class ChaosMonkey : public rclcpp::Node
{
public:
  ChaosMonkey()
  : Node("chaos_monkey")
  {
    this->declare_parameter("target_nodes_prefix", "agent_");
    this->declare_parameter("kill_interval_s", 5);
    this->declare_parameter("discovery_time_s", 10);

    // wait for discovery time
    // it's safe to assume that for a short period after startup all nodes are up and running before facing ipotetical failures
    int discovery_time = this->get_parameter("discovery_time_s").as_int();
    RCLCPP_INFO(this->get_logger(), "Chaos Monkey waiting %d seconds simulating a correct initialization", discovery_time);
    rclcpp::sleep_for(std::chrono::seconds(discovery_time));

    timer_ = this->create_wall_timer(
      std::chrono::seconds(this->get_parameter("kill_interval_s").as_int()),
      std::bind(&ChaosMonkey::kill_random_node, this));
      
    rng_.seed(std::random_device()());
  }

private:
  void kill_random_node()
  {
    std::string prefix = this->get_parameter("target_nodes_prefix").as_string();
    
    auto node_names = this->get_node_graph_interface()->get_node_names();
    std::vector<std::string> target_candidates;

    for (const auto & name : node_names) {
      if (name.find(prefix) != std::string::npos) {
        target_candidates.push_back(name);
      }
    }

    if (target_candidates.empty()) {
      RCLCPP_WARN(get_logger(), "No nodes found matching prefix '%s'", prefix.c_str());
      return;
    }
    
    // Sort candidates to ensure higher indices correspond to "higher" nodes (lexicographically)
    std::sort(target_candidates.begin(), target_candidates.end());

    // Create weights: higher index -> higher weight
    // Example: Linear weights (1, 2, 3, 4, 5...)
    std::vector<double> weights;
    for (size_t i = 0; i < target_candidates.size(); ++i) {
        weights.push_back(static_cast<double>(i + 1));
    }

    std::discrete_distribution<int> dist(weights.begin(), weights.end());
    int index = dist(rng_);
    std::string node_name = target_candidates[index];

    RCLCPP_INFO(get_logger(), "Chaos Monkey targeting: %s", node_name.c_str());
    
    std::string get_state_service_name = node_name + "/get_state";
    std::string change_state_service_name = node_name + "/change_state";

    auto get_state_client = this->create_client<lifecycle_msgs::srv::GetState>(get_state_service_name);
    auto change_state_client = this->create_client<lifecycle_msgs::srv::ChangeState>(change_state_service_name);
    
    if (!get_state_client->wait_for_service(1s) || !change_state_client->wait_for_service(1s)) {
      RCLCPP_WARN(get_logger(), "Services for %s not available. Node might be dead or not exist.", node_name.c_str());
      return;
    }

    auto request = std::make_shared<lifecycle_msgs::srv::GetState::Request>();
    
    // Capture clients to keep them alive during async calls
    get_state_client->async_send_request(request, 
      [this, node_name, get_state_client, change_state_client](rclcpp::Client<lifecycle_msgs::srv::GetState>::SharedFuture future_state) {
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
              RCLCPP_INFO(this->get_logger(), "Node %s is in state %d, cannot shutdown or already shutdown", node_name.c_str(), current_state);
              return;
          }

          auto change_req = std::make_shared<lifecycle_msgs::srv::ChangeState::Request>();
          change_req->transition.id = transition_id;
          // Do not set label, let it use ID
          // change_req->transition.label = transition_label;

          RCLCPP_INFO(this->get_logger(), "Sending %s (id %d) to %s", log_label.c_str(), transition_id, node_name.c_str());

          change_state_client->async_send_request(change_req, 
            [this, node_name, change_state_client](rclcpp::Client<lifecycle_msgs::srv::ChangeState>::SharedFuture future_change) {
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

  rclcpp::TimerBase::SharedPtr timer_;
  std::mt19937 rng_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<ChaosMonkey>());
  rclcpp::shutdown();
  return 0;
}
