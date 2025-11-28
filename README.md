# Leader Election for ROS 2: Crash Resilient Architecture

## Overview
This project aims to develop a crash-resilient architecture for ROS 2 systems by exploiting leader election theory and algorithms. The primary goal is to ensure high availability and fault tolerance in robotic applications where node failures can be critical.

## Key Objectives

1.  **Crash Resilience**: Implement robust mechanisms to detect node failures and automatically elect a new leader to take over control, ensuring continuous operation.
2.  **Algorithm Analysis**: Implement and test multiple leader election algorithms to study their impact on the ROS network.
    *   **Focus**: Analyze the number and type of messages exchanged (overhead), convergence time, and scalability.
3.  **Minimal Intrusion**: Develop a system (middleware or wrapper) to integrate leader election capabilities into existing Python or C++ nodes with as few code modifications as possible.

## Architecture

The proposed architecture involves a "Leader Election" layer that sits alongside or wraps the core application logic of a ROS node.
- **Leader Node**: The active node performing the task.
- **Follower Nodes**: Standby nodes monitoring the leader.
- **Election Process**: When a leader fails, followers trigger an election to select a new leader.

### Integration Strategy
The goal is to allow developers to make standard ROS nodes resilient by simply inheriting from a base class or using a sidecar pattern, minimizing the need to rewrite business logic.

## Supported Algorithms

The project investigates several algorithms, comparing their suitability for ROS environments (wireless, dynamic topology, etc.):

*   **Bully Algorithm**: High ID wins. Fast but high message overhead.
*   **Ring Algorithm**: Token passing or logical ring. Lower overhead but higher latency.
*   **Raft**: Consensus-based, strong consistency (Log replication).
*   **Chang and Roberts**: Optimization for ring topologies.
*   **Hirschberg-Sinclair**: Bidirectional ring election.
*   **Dynamic/Wireless**: Algorithms optimized for ad-hoc networks.

## Metrics & Analysis

We aim to benchmark these algorithms based on:
*   **Message Complexity**: Total number of messages exchanged during election.
*   **Message Size**: Bandwidth usage.
*   **Convergence Time**: Time taken to elect a new leader after failure.
*   **Stability**: Resistance to false positives (e.g., network jitter).

## Proposed Usage (Concept)

To achieve minimal intrusion, we aim for a mixin or decorator pattern.

### Python Example
```python
from leader_election.adapters import LeaderElectionAdapter

class MyNode(Node):
    def __init__(self):
        super().__init__('my_node')
        # ... existing init ...

    def do_work(self):
        # This method only runs if this node is the leader
        pass

# Wrap the node
adapter = LeaderElectionAdapter(MyNode(), algorithm='bully')
adapter.spin()
```

### C++ Example
```cpp
#include "leader_election/leader_election.hpp"

class MyNode : public rclcpp::Node {
    // ...
};

int main(int argc, char ** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<MyNode>();
    
    // Leader Election Wrapper
    LeaderElectionManager manager(node, Algorithm::RAFT);
    manager.on_become_leader([](){ /* start work */ });
    manager.on_lose_leadership([](){ /* stop work */ });
    
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
```

## ROS 2 Specific Considerations

*   **Quality of Service (QoS)**: The reliability of election messages is crucial. We will investigate how `Reliable` vs `Best Effort` QoS settings impact the algorithms, especially in wireless scenarios.
*   **Discovery**: ROS 2 discovery can be slow. Algorithms might need to account for nodes appearing/disappearing from the graph dynamically.
*   **DDS**: The underlying DDS implementation (FastDDS, CycloneDDS) might influence message latency and overhead.

## Roadmap

1.  **Phase 1**: Define standard interfaces for Leader Election (Heartbeats, Voting).
2.  **Phase 2**: Implement the **Bully Algorithm** as a baseline.
3.  **Phase 3**: Develop the **Ring Algorithm** and compare metrics.
4.  **Phase 4**: Implement **Raft** for strong consistency requirements.
5.  **Phase 5**: Create the "Sidecar/Wrapper" library for easy integration.
6.  **Phase 6**: Benchmarking and Analysis on real/simulated networks.

---

## Theoretical Background & Notes

### Assumptions
- Each node has a unique ID (PID or UUID).
- Nodes can discover each other (via ROS discovery or static map).
- **Communication**:
    - Each node publishes its PID?
    - N topics vs 1 topic with ring/append message?
    - Each node knows the map of nodes that should be active and reachable?
    - How to publish the map? Is the map hardcoded? Do we assume the start of a first leader/orchestrator?

### Algorithm Details

#### Bully Algorithm
When any process notices that the coordinator is no longer responding to requests, it initiates an election. A process, $P_k$, holds an election as follows:
1.  $P_k$ sends an ELECTION message to all processes with higher identifiers: $P_{k+1}, P_{k+2}, \dots, P_{N-1}$.
2.  If no one responds, $P_k$ wins the election and becomes coordinator.
3.  If one of the higher-ups answers, it takes over and $P_k$’s job is done.

At any moment, a process can get an ELECTION message from one of its lower-numbered colleagues. When such a message arrives, the receiver sends an OK message back to the sender to indicate that it is alive and will take over. The receiver then holds an election, unless it is already holding one. Eventually, all processes give up but one, and that one is the new coordinator.
If a process that was previously down comes back up, it holds an election.

#### Ring Algorithm
Unlike some ring algorithms, this one does not use a token. We assume that each process knows who its successor is.
When any process notices that the coordinator is not functioning, it builds an ELECTION message containing its own process identifier and sends the message to its successor.
If the successor is down, the sender skips over the successor and goes to the next member along the ring, or the one after that, until a running process is located.
At each step along the way, the sender adds its own identifier to the list in the message, effectively making itself a candidate to be elected as coordinator.
Eventually, the message gets back to the process that started it all. That process recognizes this event when it receives an incoming message containing its own identifier. At that point, the message type is changed to COORDINATOR and circulated once again, this time to inform everyone else who the coordinator is (the list member with the highest identifier) and who the members of the new ring are. When this message has circulated once, it is removed and everyone goes back to work.

#### Raft (Consensus)
Raft operates in a setting in which a handful of known replicated servers (typically five) collaborate by ensuring that each server executes the same set of operations, and all in the same order.
To this end, one of the servers is elected as leader to tell the others what the exact order is of those operations.
The protocol assumes that messages may be lost and that servers may crash.
Each server can be in one of three states: **follower**, **candidate**, or **leader**.
Furthermore, the protocol operates in terms, where during each term there is exactly one leader, although it could have perhaps crashed. Each term is numbered, starting with 0. 
The leader is assumed to regularly send out a message, either containing information on an operation that should be carried out, or otherwise a heartbeat message to tell the other servers that their leader is still up and running.

Each server initially starts in the follower state (that is, there is initially no leader). After a follower timeout, a following server concludes that the leader may have crashed (which, by the way, may be a false conclusion). As a result, it enters the candidate state and starts an election, volunteering to be the new leader. An election starts with a broadcast to all other servers, along with increasing the term number by 1. At that point, three situations may happen:
1.  A candidate may receive a message from an alleged leader. If that server indicates it is operating in the same term as the candidate server, the latter will become follower again for the current term.
2.  When a following server receives an election message for the first time (in a new term), it simply votes for the candidate and ignores any other election messages (for that new term). Therefore, a candidate server can also receive a vote. If it has a majority of votes (i.e., more than half of the servers, including itself), it promotes itself as leader for the new term.
3.  As long as there is no alleged leader, or not enough votes have been received, the candidate server waits until a candidate timeout happens. At that point, the candidate server will simply start a new election (and again, for a next term).

#### ZooKeeper (ZAB)
ZooKeeper is a logically centralized coordination service. It maintains a (relatively small) set of servers, forming what is called an ensemble. For a client, an ensemble appears as just a single server. For ZooKeeper, this ensemble is also coordinated by a single server, called the leader. The other servers are called followers and essentially act as up-to-date standbys for whenever the leader malfunctions.

When a follower $s$ believes something is wrong with the leader (e.g., it suspects that the leader crashed), it sends out an ELECTION message to all other servers, along with the pair $(voteID, voteTX)$. For this first message, it sets $voteID$ to $id(s)$ and $voteTX$ to $tx(s)$. During an election, each server $s$ maintains two variables:
1.  $leader(s)$: records the identifier of the server that $s$ believes may turn out to be the final leader, initialized to $id(s)$.
2.  $lastTX(s)$: what $s$ has learned to be the most recent transaction, initially being its own value, namely $tx(s)$.

When a server $s^*$ receives $(voteID, voteTX)$, it proceeds as follows:
*   If $lastTX(s^*) < voteTX$, then $s^*$ just received more up-to-date information on the most recent transaction. In that case it sets:
    *   $leader(s^*) \leftarrow voteID$
    *   $lastTX(s^*) \leftarrow voteTX$
*   If $lastTX(s^*) = voteTX$ and $leader(s^*) < voteID$, then $s^*$ knows as much about the most recent transaction as what it was just sent, but its perspective on which server will be the next leader needs to be updated:
    *   $leader(s^*) \leftarrow voteID$

Each time a server $s^*$ receives a $(voteID, voteTX)$ message, it may update its own information on whom it suspects to be the next leader. If $s^*$ believes it should be the next leader, and has not sent out a message stating this, it will broadcast the pair $(id(s^*), tx(s^*))$.

#### Elections in Wireless Environments
Traditional election algorithms are generally based on assumptions that are not realistic in wireless environments. For example, they assume that message passing is reliable and that the topology of the network does not change. These assumptions are false in most wireless environments, especially when dealing with mobile devices.
But in a ROS environment, we could remove the assumption that the network structure is known.
Otherwise, if we assume that the configuration is known, probably the best option is to have the first leader fixed and be him to send the configuration to the others. In that case we also talk about aggregation? Since ROS has broadcast topics, simply each follower should subscribe to the leader's config topic that is publishing.

### Fault Tolerance Concepts

Being fault tolerant is strongly related to what are called dependable systems. Dependability covers a number of useful requirements:
*   **Availability**: Defined as the property that a system is ready to be used immediately. In general, it refers to the probability that the system is operating correctly at any given moment.
*   **Reliability**: Refers to the property that a system can run continuously without failure. Defined in terms of a time interval, instead of an instant in time. A highly reliable system is one that will most likely continue to work without interruption during a relatively long period of time.
*   **Safety**: Refers to the situation that when a system temporarily fails to operate correctly, no catastrophic event happens (e.g., nuclear power plants, space systems).
*   **Maintainability**: Refers to how easily a failed system can be repaired.

#### Failure Models
To get a better grasp on how serious a failure actually is, several classification schemes have been developed:
*   **Crash Failure**: A server prematurely halts, but was working correctly until it stopped. Once halted, nothing is heard from it anymore.
*   **Omission Failure**: A server fails to respond to a request.
    *   *Receive-omission*: Server never got the request.
    *   *Send-omission*: Server failed to send a response (e.g., buffer overflow).
*   **Timing Failure**: The response lies outside a specified real-time interval (e.g., streaming video data arriving too late).
*   **Response Failure**: The server's response is simply incorrect.
    *   *Value failure*: Wrong reply to a request.
    *   *State-transition failure*: Server reacts unexpectedly to an incoming request (e.g., unrecognized message).
*   **Arbitrary (Byzantine) Failure**: Clients should be prepared for the worst. A server may produce output it should never have produced, but which cannot be detected as being incorrect.

### References
- [Leader Election Algorithms (Wikipedia)](https://en.wikipedia.org/wiki/Leader_election#Algorithms)
- [Bully Algorithm](https://en.wikipedia.org/wiki/Bully_algorithm)
- [Chang and Roberts](https://en.wikipedia.org/wiki/Chang_and_Roberts_algorithm)
- [Hirschberg-Sinclair](https://en.wikipedia.org/wiki/Hirschberg%E2%80%93Sinclair_algorithm)
- [Raft Consensus](https://raft.github.io/)
- [Distributed Computing](https://en.wikipedia.org/wiki/Distributed_computing#Election)
