#include <memory>
#include <vector>
#include <string>
#include <algorithm>

#include "rclcpp/rclcpp.hpp"
#include "distributed_election/bully_agent.hpp"
#include "lifecycle_msgs/msg/state.hpp"

#include "std_msgs/msg/int32.hpp"

class SimulationOrchestrator : public rclcpp::Node
{
public:
  SimulationOrchestrator(rclcpp::Executor * executor)
  : Node("simulation_orchestrator"), executor_(executor)
  {
    this->declare_parameter("num_agents", 3);
    this->declare_parameter("heartbeat_interval_ms", 1000);
    this->declare_parameter("nodes_name_prefix", "agent_");
    
    create_agents();
    
    cleanup_timer_ = this->create_wall_timer(
      std::chrono::seconds(1),
      std::bind(&SimulationOrchestrator::cleanup_dead_agents, this));

    rclcpp::QoS qos_profile(1); // Keep last 1
    qos_profile.best_effort();  // UDP-like behavior (faster, less overhead)
    revival_sub_ = this->create_subscription<std_msgs::msg::Int32>(
      "/election/revive", qos_profile,
      std::bind(&SimulationOrchestrator::revive_agent, this, std::placeholders::_1));
  }

  void create_agents()
  {
    int num_agents = this->get_parameter("num_agents").as_int();
    int heartbeat_interval = this->get_parameter("heartbeat_interval_ms").as_int();
    std::string nodes_name_prefix = this->get_parameter("nodes_name_prefix").as_string();

    RCLCPP_INFO(this->get_logger(), "Creating %d agents...", num_agents);

    for (int i = 0; i < num_agents; ++i) {
      spawn_agent(i, heartbeat_interval, nodes_name_prefix);
    }
  }

  void spawn_agent(int id, int heartbeat_interval, const std::string & prefix)
  {
      std::string node_name = prefix + std::to_string(id);
      auto agent = std::make_shared<distributed_election::BullyAgent>(node_name, id, heartbeat_interval);
      agents_.push_back(agent);
      
      executor_->add_node(agent->get_node_base_interface());
      
      agent->configure();
      agent->activate();
  }
  
  void revive_agent(const std_msgs::msg::Int32::SharedPtr msg)
  {
    int target_id = msg->data;
    std::string nodes_name_prefix = this->get_parameter("nodes_name_prefix").as_string();
    std::string target_name = nodes_name_prefix + std::to_string(target_id);

    // Check if agent already exists and is alive
    for (const auto & agent : agents_) {
      if (agent->get_name() == target_name) {
        if (agent->get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED) {
           // RCLCPP_INFO(this->get_logger(), "Revival request for %s ignored: Agent is still alive/active.", target_name.c_str());
           return;
        }
      }
    }

    RCLCPP_INFO(this->get_logger(), "Reviving agent %d...", target_id);
    int heartbeat_interval = this->get_parameter("heartbeat_interval_ms").as_int();
    spawn_agent(target_id, heartbeat_interval, nodes_name_prefix);
  }
  
  void cleanup_dead_agents()
  {
    auto it = agents_.begin();
    while (it != agents_.end()) {
      auto & agent = *it;
      if (agent->get_current_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED) {
        // RCLCPP_INFO(this->get_logger(), "Agent %s is finalized. Removing from simulation.", agent->get_name());
        executor_->remove_node(agent->get_node_base_interface());
        it = agents_.erase(it);
      } else {
        ++it;
      }
    }
  }

private:
  rclcpp::Executor * executor_;
  std::vector<std::shared_ptr<distributed_election::BullyAgent>> agents_;
  rclcpp::TimerBase::SharedPtr cleanup_timer_;
  rclcpp::Subscription<std_msgs::msg::Int32>::SharedPtr revival_sub_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  rclcpp::executors::MultiThreadedExecutor executor;
  auto orchestrator = std::make_shared<SimulationOrchestrator>(&executor);
  
  executor.add_node(orchestrator);

  RCLCPP_INFO(orchestrator->get_logger(), "Simulation started. Spinning nodes...");
  executor.spin();

  rclcpp::shutdown();
  return 0;
}
