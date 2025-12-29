#include <memory>
#include <vector>
#include <string>
#include <algorithm>

#include "rclcpp/rclcpp.hpp"
#include "distributed_election/simple_agent.hpp"
#include "lifecycle_msgs/msg/state.hpp"

class SimulationOrchestrator : public rclcpp::Node
{
public:
  SimulationOrchestrator(rclcpp::Executor * executor)
  : Node("simulation_orchestrator"), executor_(executor)
  {
    this->declare_parameter("num_agents", 3);
    this->declare_parameter("heartbeat_interval_ms", 1000);
    
    create_agents();
    
    // Check for dead agents every 1 second
    cleanup_timer_ = this->create_wall_timer(
      std::chrono::seconds(1),
      std::bind(&SimulationOrchestrator::cleanup_dead_agents, this));
  }

  void create_agents()
  {
    int num_agents = this->get_parameter("num_agents").as_int();
    int heartbeat_interval = this->get_parameter("heartbeat_interval_ms").as_int();

    RCLCPP_INFO(this->get_logger(), "Creating %d agents...", num_agents);

    for (int i = 0; i < num_agents; ++i) {
      std::string node_name = "agent_" + std::to_string(i);
      auto agent = std::make_shared<distributed_election::SimpleAgent>(node_name, i, heartbeat_interval);
      agents_.push_back(agent);
      
      // Add to executor
      executor_->add_node(agent->get_node_base_interface());
      
      // Auto-configure and activate for simulation purposes
      agent->configure();
      agent->activate();
    }
  }
  
  void cleanup_dead_agents()
  {
    auto it = agents_.begin();
    while (it != agents_.end()) {
      auto & agent = *it;
      if (agent->get_current_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED) {
        RCLCPP_INFO(this->get_logger(), "Agent %s is finalized. Removing from simulation.", agent->get_name());
        executor_->remove_node(agent->get_node_base_interface());
        it = agents_.erase(it);
      } else {
        ++it;
      }
    }
  }

private:
  rclcpp::Executor * executor_;
  std::vector<std::shared_ptr<distributed_election::SimpleAgent>> agents_;
  rclcpp::TimerBase::SharedPtr cleanup_timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  rclcpp::executors::MultiThreadedExecutor executor;
  auto orchestrator = std::make_shared<SimulationOrchestrator>(&executor);
  
  executor.add_node(orchestrator);

  RCLCPP_INFO(orchestrator->get_logger(), "Simulation started. Spinning...");
  executor.spin();

  rclcpp::shutdown();
  return 0;
}
