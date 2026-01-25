#include "distributed_election/raft_agent.hpp"
#include <algorithm>

namespace distributed_election
{

RaftAgent::RaftAgent(const std::string & node_name, int id, int heartbeat_interval_ms, int heartbeat_max_tick)
: SimpleAgent(node_name, id, heartbeat_interval_ms, heartbeat_max_tick)
{
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RaftAgent::on_configure(const rclcpp_lifecycle::State & state)
{
  // ----------------------------------- Simple Agent Setup -----------------------------------
  auto result = SimpleAgent::on_configure(state);
  if (result != rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS) {
    return result;
  }

  // ----------------------------------- Ring Agent Setup -----------------------------------
  rclcpp::QoS qos_profile(1);
  qos_profile.best_effort();
  
  vote_pub_ = this->create_publisher<std_msgs::msg::Int32MultiArray>("/election/vote", qos_profile);

  vote_sub_ = this->create_subscription<std_msgs::msg::Int32MultiArray>(
    "/election/vote", 
    qos_profile, 
    std::bind(&RaftAgent::on_vote_received, this, std::placeholders::_1));

  startup_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * heartbeat_max_tick_ / 3),
    std::bind(&RaftAgent::on_startup_timer, this));

  election_watchdog_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(heartbeat_interval_ms_ * heartbeat_max_tick_ / 2),
    std::bind(&RaftAgent::on_watchdog_timeout, this));
  election_watchdog_timer_->cancel();

  heartbeat_timer_->cancel();


  return rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn::SUCCESS;
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RaftAgent::on_activate(const rclcpp_lifecycle::State & state)
{
  leader_id_ = -1;
  is_election_in_progress_ = false;
  was_i_the_initiator_ = false;
  election_ready_ = false;

  return SimpleAgent::on_activate(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RaftAgent::on_deactivate(const rclcpp_lifecycle::State & state)
{
  if (startup_timer_) startup_timer_->cancel();
  return SimpleAgent::on_deactivate(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RaftAgent::on_cleanup(const rclcpp_lifecycle::State & state)
{
  startup_timer_.reset();
  vote_pub_.reset();
  vote_sub_.reset();
  return SimpleAgent::on_cleanup(state);
}

rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn
RaftAgent::on_shutdown(const rclcpp_lifecycle::State & state)
{
  startup_timer_.reset();
  vote_pub_.reset();
  vote_sub_.reset();
  return SimpleAgent::on_shutdown(state);
}

void RaftAgent::on_startup_timer()
{
  if (!election_ready_) {
    election_ready_ = true;
    announce_heartbeat();
    // RCLCPP_INFO(get_logger(), "Agent %d is ready for election.", id_);
    return;
  }
  if (leader_id_ != -1){
    startup_timer_->cancel();
    heartbeat_timer_->reset();
    RCLCPP_INFO(get_logger(), "Agent %d started.", id_);
    return;
  }
  announce_heartbeat();

  if (!is_election_in_progress_) run_election_logic();

  RCLCPP_INFO(get_logger(), "Agent %d started and waiting for leader election to complete.", id_);
}

void RaftAgent::on_heartbeat() 
{
  publish_heartbeat();

  rclcpp::Time now = this->now();

  // remove agents from heartbeat tick map if they are no longer active (last seen > heartbeat_interval * max_tick)
  std::vector<int> failed_agents;
  for (const auto & entry : last_heartbeat_tick_map_) {
    int other_id = entry.first;
    if (other_id == id_) continue;
    rclcpp::Time last_seen = last_heartbeat_map_[other_id];
    if ((now - last_seen).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * heartbeat_max_tick_) {
      failed_agents.push_back(other_id);
    }
  }
  for (const auto & rid : failed_agents) {
    if (suspected_dead_agents_.find(rid) == suspected_dead_agents_.end()) {
      suspected_dead_agents_[rid] = now;
      if (leader_id_ == id_) {
        RCLCPP_WARN(get_logger(), "Leader %d detected failure of Agent %d, reviving.", id_, rid);
        revive_agent(rid);
      }
    }
    last_heartbeat_tick_map_.erase(rid);
  }

  // Check leader heartbeat
  if (leader_id_ != id_){
    if (last_heartbeat_map_.find(leader_id_) == last_heartbeat_map_.end()) {
      // Never received heartbeat from leader
      if (!is_election_in_progress_) {
        RCLCPP_WARN(get_logger(), "Agent %d has never received heartbeat from leader Agent %d", id_, leader_id_);
        run_election_logic(); 
      }
      return;
    }else if ((now - last_heartbeat_map_[leader_id_]).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * heartbeat_max_tick_) {
      if (!is_election_in_progress_) {
        RCLCPP_WARN(get_logger(), "Agent %d detected failure of leader Agent %d", id_, leader_id_);
        run_election_logic(); 
      }
      return;
    }
  }else{
    // Check if any dead agent is not reviving
    for (auto it = suspected_dead_agents_.begin(); it != suspected_dead_agents_.end(); ) {
      int agent = it->first;
      if ((now - it->second).nanoseconds() * 1e-6 > 3 * heartbeat_interval_ms_ * heartbeat_max_tick_) {
        RCLCPP_WARN(get_logger(), "Agent %d not revived, retrying...", agent);
        revive_agent(agent);
        it->second = now;
      }
      ++it;
    }
    // check for nodes lost and removed from suspected dead agents, but still present in last_heartbeat_map_
    // notes: last_heartbeat_tick_map_ is pruned when an agent is considered failed, but this is expected the compete network will be saved in last_heartbeat_map_
    for (const auto & entry : last_heartbeat_map_) {
      int other_id = entry.first;
      if (other_id == id_) continue;
      if (suspected_dead_agents_.find(other_id) == suspected_dead_agents_.end()) {
        rclcpp::Time last_seen = entry.second;
        if ((now - last_seen).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * heartbeat_max_tick_) {
          suspected_dead_agents_[other_id] = now;
          RCLCPP_WARN(get_logger(), "Leader %d detected failure of Agent %d from heartbeat, reviving.", id_, other_id);
          revive_agent(other_id);
        }
      }
    }
  }
}

void RaftAgent::on_heartbeat_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (msg->data.empty()) return;

  // safety_check:
  if (msg->data[0] == -1) return; // invalid announce heartbeat
  // someone impersonating an agent with id_ -1 can be a security edge case, keep in mind...

  if (last_heartbeat_map_.find(msg->data[0]) == last_heartbeat_map_.end()) {
    // RCLCPP_INFO(get_logger(), "Agent %d noticed new agent %d", id_, msg->data[0]);
  }
  last_heartbeat_map_[msg->data[0]] = this->now();
  // RCLCPP_INFO(get_logger(), "Agent %d received heartbeat from Agent %d", id_, msg->data[0]);

  if (suspected_dead_agents_.count(msg->data[0])) {
    suspected_dead_agents_.erase(msg->data[0]);
    //RCLCPP_INFO(get_logger(), "Agent %d recovered.", msg->data[0]);
  }

  // to elect the most reliabale leader, keep track of heartbeats in a counter-based map
  if (last_heartbeat_tick_map_.find(msg->data[0]) == last_heartbeat_tick_map_.end()) {
    last_heartbeat_tick_map_[msg->data[0]] = 0;
  }else{
    last_heartbeat_tick_map_[msg->data[0]] += 1;
  }
}

int RaftAgent::choose_leader()
{
  // to elect the most reliabale leader, pick the agent with most heartbeats ticks, 
  // in case of a tie most recent heartbeat wins, higher ID wins
  std::vector<std::pair<int, int>> candidates; // pair of (agent_id, heartbeat_ticks)
  for (const auto & entry : last_heartbeat_tick_map_) {
    candidates.push_back(std::make_pair(entry.first, entry.second));
  }
  // sort by heartbeat ticks desc, last seen desc, id desc
  std::sort(candidates.begin(), candidates.end(), [this](const std::pair<int, int> & a, const std::pair<int, int> & b) {
    if (a.second != b.second) {
      return a.second > b.second; // more heartbeat ticks
    }else{
      rclcpp::Time a_last_seen = last_heartbeat_map_[a.first];
      rclcpp::Time b_last_seen = last_heartbeat_map_[b.first];
      if (a_last_seen != b_last_seen) {
        return a_last_seen > b_last_seen; // more recent heartbeat
      }else{
        return a.first > b.first; // higher ID
      }
    }
  });   
  
  if (!candidates.empty()) {
    return candidates[0].first;
  }else{
    return -1;
  }
}

void RaftAgent::run_election_logic()
{  
  election_map_.clear();
  
  if (!is_election_in_progress_) {
    // I am starting the election/watchdog, so I am the initiator
    election_watchdog_timer_->reset();
    was_i_the_initiator_ = true;
  }
  is_election_in_progress_ = true;
  RCLCPP_INFO(get_logger(), "Agent %d initiating Raft Election", id_);

  int candidate_id = choose_leader();
  if (candidate_id == -1) {
    RCLCPP_WARN(get_logger(), "Agent %d found no suitable leader candidates", id_);
    return;
  }
  
  // broadcast vote request
  std_msgs::msg::Int32MultiArray msg;
  msg.data.push_back(candidate_id);
  vote_pub_->publish(msg);
}

void RaftAgent::on_vote_received(const std_msgs::msg::Int32MultiArray::SharedPtr msg)
{
  if (leader_id_ == id_) { // if i'm the leader, re-announce
    publish_leader();
    return;
  }

  // If I am NOT in an election state at all, this means I am hearing about an election for the first time.
  // I should enter the "election in progress" state (to prevent voting multiple times) but NOT start a watchdog (timer).
  if (!is_election_in_progress_) {
    is_election_in_progress_ = true;
    was_i_the_initiator_ = false; // I am a follower/voter in this election
    election_watchdog_timer_->reset(); // Safety timeout for voters

    int candidate_id = choose_leader();
    if (candidate_id == -1) {
      RCLCPP_WARN(get_logger(), "Agent %d found no suitable leader candidates", id_);
      return;
    }
  
    // Broadcast my vote response
    std_msgs::msg::Int32MultiArray response_msg;
    response_msg.data.push_back(candidate_id);
    vote_pub_->publish(response_msg);
    return;
  }

  // If I am already in an election state:
  // 1. If I am a follower (was_i_the_initiator_ == false), I have already voted, so I ignore further vote messages.
  if (!was_i_the_initiator_) return;

  // 2. If I am the initiator (was_i_the_initiator_ == true), I need to COUNT the votes.
  // simply add votes to the elction map
  if (msg->data.size() != 1) return; // invalid vote message
  int candidate_id = msg->data[0];
  election_map_[candidate_id] += 1;
}

void RaftAgent::on_watchdog_timeout()
{
  // the election period ended
  election_watchdog_timer_->cancel();
  is_election_in_progress_ = false;

  if (!was_i_the_initiator_) {
    // If I was just a voter, this timeout means the election finished without me receiving a leader update.
    // I reset my state so I can participate in or start new elections.
    return;
  }

  int max_votes = 0;
  int new_leader = -1;
  for (const auto & entry : election_map_) {
    int candidate_id = entry.first;
    int votes = entry.second;
    if (votes > max_votes) {
      max_votes = votes;
      new_leader = candidate_id;
    } else if (votes == max_votes) {
      // tie, higher id wins
      if (candidate_id > new_leader) {
        new_leader = candidate_id;
      }
    }
  }
  if (new_leader != -1) {
    leader_id_ = new_leader;
    publish_leader();
    was_i_the_initiator_ = true;
    RCLCPP_INFO(get_logger(), "Agent %d elected %d as leader", id_, leader_id_);
  } else {
    RCLCPP_WARN(get_logger(), "No leader elected");
  }
  if (leader_id_ == id_) {
    announce_heartbeat();
  }
}

void RaftAgent::on_leader_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  if (leader_id_ == msg->data) return; // already know this leader

  election_watchdog_timer_->cancel();
  if(!was_i_the_initiator_) {
    if (is_election_in_progress_) {
      is_election_in_progress_ = false;
      election_map_.clear();
    }
    leader_id_ = msg->data;
    if (leader_id_ == id_) {
      announce_heartbeat();
    }
  }else{
    // optimisation: i was the initiator, but someone else announced a leader
    // this will prevent uselsess election untill the current leader fails
    is_election_in_progress_ = false;
    was_i_the_initiator_ = false;
    election_map_.clear();
    leader_id_ = msg->data;
  }
  RCLCPP_INFO(get_logger(), "Agent %d acknowledges new leader: Agent %d", id_, leader_id_);
}

}  // namespace distributed_election
