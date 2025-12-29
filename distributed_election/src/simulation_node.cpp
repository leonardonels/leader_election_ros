#include <memory>
#include <vector>
#include <string>

#include "rclcpp/rclcpp.hpp"
#include "distributed_election/simple_agent.hpp"

class SimulationOrchestrator : public rclcpp::Node
{
public:
  SimulationOrchestrator()
  : Node("simulation_orchestrator")
  {
    this->declare_parameter("num_agents", 3);
    this->declare_parameter("heartbeat_interval_ms", 1000);
  }

  void create_agents(std::vector<std::shared_ptr<distributed_election::SimpleAgent>> & agents)
  {
    int num_agents = this->get_parameter("num_agents").as_int();
    int heartbeat_interval = this->get_parameter("heartbeat_interval_ms").as_int();

    RCLCPP_INFO(this->get_logger(), "Creating %d agents...", num_agents);

    for (int i = 0; i < num_agents; ++i) {
      std::string node_name = "agent_" + std::to_string(i);
      auto agent = std::make_shared<distributed_election::SimpleAgent>(node_name, i, heartbeat_interval);
      agents.push_back(agent);
      
      agent->configure();
      agent->activate();
    }
  }
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  auto orchestrator = std::make_shared<SimulationOrchestrator>();
  
  std::vector<std::shared_ptr<distributed_election::SimpleAgent>> agents;
  orchestrator->create_agents(agents);

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(orchestrator);
  
  for (auto & agent : agents) {
    executor.add_node(agent->get_node_base_interface());
  }

  RCLCPP_INFO(orchestrator->get_logger(), "Simulation started. Spinning...");
  executor.spin();

  rclcpp::shutdown();
  return 0;
}
