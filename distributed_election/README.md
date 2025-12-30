# Distributed Leader Election Simulation (ROS 2)

This package implements a distributed leader election simulation using ROS 2 Lifecycle Nodes.

## Overview

The system simulates a cluster of distributed agents that must agree on a leader. To make the simulation interesting, a "Chaos Monkey" randomly kills nodes, forcing the remaining agents to detect the failure and elect a new leader. A central Orchestrator manages the lifecycle of the agents and revives them when requested.

### Key Features
- **Lifecycle Nodes**: Agents are implemented as `rclcpp_lifecycle` nodes, allowing managed states (Unconfigured, Inactive, Active, Finalized).
- **Bully Algorithm**: Implements the classic Bully Algorithm where the node with the highest ID claims leadership.
- **Failure Detection**: Agents monitor heartbeats to detect leader or peer failures.
- **Chaos Monkey**: A separate node that randomly "kills" (transitions to Finalized state) agents to simulate crashes.
- **Auto-Revival**: Surviving nodes detect failures and request the Orchestrator to revive dead agents, simulating system recovery.

## Architecture

### Nodes

1.  **`simulation_orchestrator`**:
    -   The main entry point.
    -   Spawns `N` agent nodes within a single process using a `MultithreadedExecutor`.
    -   Monitors the `/election/revive` topic to respawn agents that have been killed.
    -   Cleans up "Finalized" nodes to free resources.

2.  **`chaos_monkey`**:
    -   Periodically scans for active agent nodes.
    -   Randomly selects a victim to "kill" by calling its `ChangeState` service to transition it to `FINALIZED`.
    -   Uses a weighted distribution to prefer killing higher-ID nodes (which are usually leaders), triggering more frequent elections.

3.  **Agents (`BullyAgent` inherits `SimpleAgent`)**:
    -   **Heartbeats**: Publishes ID to `/election/heartbeats` to announce presence.
    -   **Election**: Listens to `/election/leader`. If a leader with a lower ID is seen, or the leader dies, it initiates an election.
    -   **Health Check**: Monitors peers. If a peer is missing for too long, it publishes to `/election/revive`.

### Topics

| Topic | Type | Description |
|-------|------|-------------|
| `/election/heartbeats` | `std_msgs/msg/Int32` | Agents broadcast their ID periodically. |
| `/election/leader` | `std_msgs/msg/Int32` | The current leader broadcasts its ID. |
| `/election/revive` | `std_msgs/msg/Int32` | Agents request the Orchestrator to revive a dead node ID. |

## Dependencies

- ROS 2 (Humble/Iron/Rolling)
- `rclcpp`
- `rclcpp_lifecycle`
- `lifecycle_msgs`
- `std_msgs`

## Build

```bash
cd ~/ros2_ws
colcon build --packages-select distributed_election
source install/setup.bash
```

## Usage

Run the simulation using the provided launch file:

```bash
ros2 launch distributed_election simulation.launch.py
```

### Configuration

You can modify the simulation parameters in `config/config.yml`:

```yaml
simulation_orchestrator:
  ros__parameters:
    num_agents: 5                 # Number of agents to spawn
    heartbeat_interval_ms: 300    # Heartbeat frequency
    nodes_name_prefix: "agent_"   # Naming prefix

chaos_monkey:
  ros__parameters:
    kill_interval_s: 10            # How often to kill a node
    discovery_time_s: 5           # Grace period at startup
    target_nodes_prefix: "agent_"
```

## How it works

1.  **Startup**: The Orchestrator spawns 5 agents (IDs 0-4). They configure and activate.
2.  **Election**: Agents discover each other via heartbeats. Agent 4 (highest ID) declares itself leader.
3.  **Chaos**: The Chaos Monkey wakes up and kills Agent 4.
4.  **Detection**: Agents 0-3 stop receiving heartbeats from Agent 4.
5.  **Re-Election**: Agent 3 detects the leader is gone and declares itself the new leader.
6.  **Revival**: Agents notice Agent 4 is missing from the heartbeat map and publish a request to `/election/revive`.
7.  **Recovery**: The Orchestrator receives the request and respawns Agent 4.
8.  **Takeover**: Agent 4 comes back online, sees Agent 3 is leader, and (being a Bully) takes over leadership again.
