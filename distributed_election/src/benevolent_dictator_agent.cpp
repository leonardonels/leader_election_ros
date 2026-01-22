#include "distributed_election/benevolent_dictator_agent.hpp"
#include <random>

namespace distributed_election
{

BenevolentDictatorAgent::BenevolentDictatorAgent(const std::string & node_name, int id, int heartbeat_interval_ms, int heartbeat_max_tick)
: SimpleAgent(node_name, id, heartbeat_interval_ms, heartbeat_max_tick),
  leader_revive_sent_(false)
{
}

SimpleAgent::CallbackReturn BenevolentDictatorAgent::on_configure(const rclcpp_lifecycle::State & state)
{
  auto ret = SimpleAgent::on_configure(state);
  if (ret != SimpleAgent::CallbackReturn::SUCCESS) {
    return ret;
  }

  // Subscribe to revive topic to sniff traffic
  rclcpp::QoS qos_profile(1);
  qos_profile.best_effort();
  revive_sub_ = this->create_subscription<std_msgs::msg::Int32>(
    "/election/revive", 
    qos_profile, 
    std::bind(&BenevolentDictatorAgent::on_revive_received, this, std::placeholders::_1));

  // Create watchdog timer (initially cancelled)
  // Timeout: ample time for restart. 
  // Using 30 * heartbeat interval (e.g., 3s for 100ms HB) to give ample time for revival.
  int watchdog_ms = heartbeat_interval_ms_ * 30; 
  watchdog_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(watchdog_ms),
    std::bind(&BenevolentDictatorAgent::on_watchdog_timeout, this));
  watchdog_timer_->cancel();

  return SimpleAgent::CallbackReturn::SUCCESS;
}

SimpleAgent::CallbackReturn BenevolentDictatorAgent::on_cleanup(const rclcpp_lifecycle::State & state)
{
  revive_sub_.reset();
  watchdog_timer_.reset();
  revival_delay_timer_.reset();
  return SimpleAgent::on_cleanup(state);
}

void BenevolentDictatorAgent::on_revive_received(const std_msgs::msg::Int32::SharedPtr msg)
{
  if (msg->data == 0) {
    // Traffic trying to revive 0 detected.
    leader_revive_sent_ = true;
    
    // Reset watchdog: cancel then reset starts it from 0
    watchdog_timer_->cancel();
    watchdog_timer_->reset();
  }
}

void BenevolentDictatorAgent::on_watchdog_timeout()
{
  // Watchdog ended.
  // If the leader has been correctly revived, we just reset the flag.
  // If not, on_heartbeat will catch it and try to revive again.
  leader_revive_sent_ = false;
  watchdog_timer_->cancel();
}

void BenevolentDictatorAgent::on_heartbeat()
{
  publish_heartbeat();

  rclcpp::Time now = this->now();
  leader_id_ = 0; // Hardcoded leader 0

  if (id_ == 0) {
    // I am the Benevolent Dictator. Keep everyone alive.
    for (const auto & agent : last_heartbeat_map_) {
      if (agent.first == id_) continue;
      
      // Check if agent is dead
      if ((now - agent.second).nanoseconds() * 1e-6 > heartbeat_interval_ms_ * heartbeat_max_tick_) {
        // Revive agent
        revive_agent(agent.first);
        RCLCPP_INFO(get_logger(), "Leader Agent %d detected failure of Agent %d. Reviving...", id_, agent.first);
        
        // Reset heartbeat timer to give the agent a grace period to wake up
        // This prevents the leader from spamming revive requests in the next cycle
        // while the agent is still booting.
        last_heartbeat_map_[agent.first] = now;
      }
    }
  } else {
    // I am a subject. Check leader 0 status.
    bool leader_alive = false;
    
    if (last_heartbeat_map_.find(0) != last_heartbeat_map_.end()) {
        double time_diff = (now - last_heartbeat_map_[0]).nanoseconds() * 1e-6;
        if (time_diff <= heartbeat_interval_ms_ * heartbeat_max_tick_) {
          leader_alive = true;
        }
    }
    
    if (!leader_alive && !leader_revive_sent_) {
      // Leader is dead and no revival traffic pending/watchdog active
      
      // Add a small random delay to reduce race conditions
      // Random delay between 0 and 200 ms
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> distrib(0, 200);
      int delay_ms = distrib(gen);

      // RCLCPP_WARN(get_logger(), "Agent %d detected failure of Leader 0. Scheduling revival in %d ms...", id_, delay_ms);

      revival_delay_timer_ = this->create_wall_timer(
        std::chrono::milliseconds(delay_ms),
        [this]() {
          this->revival_delay_timer_->cancel(); // Execute once
          if (!this->leader_revive_sent_) {
            this->revive_agent(0);
            RCLCPP_WARN(this->get_logger(), "Agent %d detected failure of Leader 0. Reviving...", this->id_);
          }
        });
      
      // Note: We will receive our own revive message in on_revive_received via the subscription,
      // which will set leader_revive_sent_ = true and start the watchdog.
    }
  }
}

} // namespace distributed_election
