#include "distributed_election/simple_agent.hpp"

namespace distributed_election
{
class BullyAgent : public SimpleAgent
{
public:
  BullyAgent(const std::string & node_name, int id, int heartbeat_interval_ms);

protected:
  void on_leader_received(const std_msgs::msg::Int32::SharedPtr msg) override;
  void run_election_logic() override;
};
}