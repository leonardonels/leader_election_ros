# Distributed Leader Election Simulation (ROS 2)

This package implements a distributed leader election simulation using ROS 2 Lifecycle Nodes. It demonstrates various consensus and election algorithms within a resilient multi-agent system.

## Overview

The system simulates a cluster of distributed agents that must agree on a leader. A "Chaos Monkey" randomly kills nodes, forcing the remaining agents to detect the failure and elect a new leader. A central Orchestrator manages the lifecycle of the agents and revives them when requested.

### Key Features
- **Lifecycle Nodes**: Agents are implemented as `rclcpp_lifecycle` nodes, allowing managed states (Unconfigured, Inactive, Active, Finalized).
- **Multiple Algorithms**: Implementations of Bully, Ring, Raft, and specialized Dictator algorithms.
- **Failure Detection**: Agents monitor heartbeats/traffic to detect leader or peer failures.
- **Chaos Monkey**: A separate node that randomly "kills" agents to simulate crashes.
- **Auto-Revival**: Automated recovery where surviving nodes request the revival of dead peers.

## Agents Implemented

The following agents are available for simulation. All agents inherit from a common **`SimpleAgent`** base class, which provides the fundamental infrastructure for lifecycle management, heartbeat broadcasting, and peer discovery (`/election/heartbeats`), but is not itself a deployable agent strategy.

### 1. BullyAgent
**Core Mechanism**: The "Bully" algorithm where the node with the highest ID always wins.

*   **Assumptions**: All nodes have unique, comparable IDs.
*   **Election Trigger**: 
    1.  Startup.
    2.  Leader failure detection (heartbeat timeout).
    3.  Receiving a leader announcement from a node with a lower ID.
*   **Logic**: 
    *   **Aggressive Takeover**: If the agent sees a leader with an ID lower than its own, it immediately declares itself the new leader and broadcasts this to the network.
    *   **Startup Check Optimisation**: On boot, it waits briefly to see if a higher-ID node is already active. If not, it self-proclaims leadership.
*   **Gossip**: To ensure network consistency, the leader periodically broadcasts its view of the network map on `/election/map` so new nodes can quickly sync up.

### 2. RingAgent
**Core Mechanism**: Logical ring topology with token passing.

*   **Topology**: Nodes auto-organize into a ring based on their IDs (e.g., 0 -> 1 -> 2 -> 0). Each node calculates its "successor" dynamically based on the list of active peers.
*   **Logic**:
    *   **Token Passing**: Election tokens are passed from a node to its successor.
    *   **Leader Selection**: The token collects information about the highest ID seen so far as it traverses the ring. When it returns to the initiator, the winner is announced.
*   **Failure Handling**: If a successor dies (heartbeat timeout), the ring is repaired by skipping the dead node and connecting to the next available successor.

### 3. HybridRingAgent
**Core Mechanism**: A combination of Ring coordination and Gossip discovery.

*   **Hybrid Approach**:
    *   **Coordination**: Uses the efficient token-passing mechanism of the Ring algorithm for elections and collision avoidance.
    *   **Discovery**: Uses the Gossip protocol (broadcasting maps) from the Bully algorithm to propagate state changes faster than a pure ring could.
*   **Benefit**: This attempts to balance the low message overhead of rings with the high resilience and fast convergence of gossip data.

### 5. RaftAgent or to be more precise Raft-Inspired Agent (Reliability-Based Voting)
**Core Mechanism**: A consensus-based approach where agents vote for the most reliable peer.

*   **Logic**:
    *   **Nomination**: When a leader failure is detected, an agent calculates the "reliability" of known peers based on the number of stable heartbeats received. It then broadcasts a vote for the most reliable candidate (which may or may not be itself).
    *   **Voting**: Votes are collected on the `/election/vote` topic during an election window.
    *   **Plurality Win**: When the election timer expires, the candidate with the most votes is declared the new leader.
*   **Difference from Standard Raft**: This implementation simplifies Raft by removing election terms and log replication, replacing the "RequestVote" RPC with a broadcast nomination based on uptime/stability metrics.

### 5. BenevolentDictatorAgent
**Core Mechanism**: Static, priority-based leadership (Node 0 is always King).

*   **Goal**: Ensure Node 0 is the leader whenever it is alive.
*   **Leader Role (Node 0)**: Its sole job is to stay alive and revive any other node that dies.
*   **Follower Role**:
    *   They constantly check if Node 0 is alive.
    *   If Node 0 dies, they do **not** try to become leader. Instead, they schedule a revival request to bring Node 0 back.
    *   **Random Backoff**: To prevent network storms where every follower tries to revive the leader simultaneously, they wait a random amount of time before sending the revive request.

## Architecture

### Components

1.  **`simulation_orchestrator`**:
    -   Spawns `N` agent nodes within a single process.
    -   Monitors `/election/revive` to respawn killed agents.
    -   Manages the simulation lifecycle.

2.  **`chaos_monkey`**:
    -   Randomly "kills" (transitions to Finalized) active agents.
    -   Can be configured to target leaders or specific subsets of nodes.

3.  **Agents**:
    -   Independent ROS 2 Lifecycle nodes running one of the implemented algorithms.

### Key Topics

| Topic | Type | Description |
|-------|------|-------------|
| `/election/heartbeats` | `std_msgs/msg/Int32MultiArray` | Agents broadcast presence. |
| `/election/leader` | `std_msgs/msg/Int32` | Current leader ID. |
| `/election/revive` | `std_msgs/msg/Int32` | Request to revive a dead node. |
| `/election/map` | `std_msgs/msg/Int32MultiArray` | Network topology (Bully/Ring). |
| `/election/ring_token` | `std_msgs/msg/Int32MultiArray` | Token passing (Ring). |
| `/election/vote` | `std_msgs/msg/Int32MultiArray` | Voting messages (Raft). |

## Build

```bash
cd ~/ros2_ws
colcon build --packages-select distributed_election_simulation
source install/setup.bash
```

## Usage

Run the simulation using the provided launch file:

```bash
ros2 launch distributed_election_simulation simulation.launch.py
```

### Configuration

Modify `config/config.yml` to change parameters or the agent type (if supported by the launch file logic).

```yaml
simulation_orchestrator:
  ros__parameters:
    num_agents: 5
    heartbeat_interval_ms: 300
    # ... other params
```

## How it works (Example)

1.  **Startup**: Agents spawn and attempt to discover peers.
2.  **Election**: Based on the chosen algorithm (e.g., Bully or Raft), an election process triggers.
3.  **Consensus**: A leader is elected and begins broadcasting on`/election/leader`.
4.  **Chaos**: Chaos Monkey terminates a random node (often the leader).
5.  **Recovery**: 
    -   Remaining agents detect the timeout.
    -   They trigger a new election.
    -   They request the Orchestrator to revive the dead node.
