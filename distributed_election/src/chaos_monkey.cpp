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
    this->declare_parameter("max_nodes", 10);

    timer_ = this->create_wall_timer(
      std::chrono::seconds(this->get_parameter("kill_interval_s").as_int()),
      std::bind(&ChaosMonkey::kill_random_node, this));
      
    rng_.seed(std::random_device()());
  }

private:
  void kill_random_node()
  {
    std::string prefix = this->get_parameter("target_nodes_prefix").as_string();
    int max_nodes = this->get_parameter("max_nodes").as_int();
    
    std::uniform_int_distribution<int> dist(0, max_nodes - 1);
    int target_id = dist(rng_);
    std::string node_name = prefix + std::to_string(target_id);

    RCLCPP_INFO(get_logger(), "Chaos Monkey targeting: %s", node_name.c_str());
    
    std::string get_state_service_name = node_name + "/get_state";
    std::string change_state_service_name = node_name + "/change_state";

    auto get_state_client = this->create_client<lifecycle_msgs::srv::GetState>(get_state_service_name);
    
    if (!get_state_client->wait_for_service(1s)) {
      RCLCPP_WARN(get_logger(), "Service %s not available. Node might be dead or not exist.", get_state_service_name.c_str());
      return;
    }

    auto request = std::make_shared<lifecycle_msgs::srv::GetState::Request>();
    auto future = get_state_client->async_send_request(request);
    
    auto change_state_client = this->create_client<lifecycle_msgs::srv::ChangeState>(change_state_service_name);
    if (!change_state_client->wait_for_service(1s)) {
       // Node likely doesn't exist
       return;
    }

    auto change_req = std::make_shared<lifecycle_msgs::srv::ChangeState::Request>();
    change_req->transition.id = lifecycle_msgs::msg::Transition::TRANSITION_ACTIVE_SHUTDOWN; 
    
    change_req->transition.id = lifecycle_msgs::msg::Transition::TRANSITION_DEACTIVATE;
    
    RCLCPP_INFO(get_logger(), "Sending DEACTIVATE to %s", node_name.c_str());
    change_state_client->async_send_request(change_req, 
      [this, node_name](rclcpp::Client<lifecycle_msgs::srv::ChangeState>::SharedFuture future) {
        try {
          auto result = future.get();
          if (result->success) {
             RCLCPP_INFO(this->get_logger(), "Successfully deactivated %s", node_name.c_str());
          } else {
             RCLCPP_WARN(this->get_logger(), "Failed to deactivate %s", node_name.c_str());
          }
        } catch (...) {
          RCLCPP_ERROR(this->get_logger(), "Service call failed for %s", node_name.c_str());
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
