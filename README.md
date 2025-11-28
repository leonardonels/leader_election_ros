# assumptions:
- each node knows its PID
  - python: ...
  - cpp: ...

- [leader election algorithms](https://en.wikipedia.org/wiki/Leader_election#Algorithms):
  - [bully algorithm](https://en.wikipedia.org/wiki/Bully_algorithm)
  - ring
  - [chang and roberts](https://en.wikipedia.org/wiki/Chang_and_Roberts_algorithm)
  - [hirshberg-sinclair](https://en.wikipedia.org/wiki/Hirschberg%E2%80%93Sinclair_algorithm)
  - dynamic (wireless) leader election
  - [raft](https://github.com/ros-raft/ros_raft.git)

- [voting system](https://en.wikipedia.org/wiki/Electoral_system)
- [distribute computing](https://en.wikipedia.org/wiki/Distributed_computing#Election)
- leader election vs aggregation?

- each node publishes its PID
  - n topics vs 1 topic with ring/append message
- each node can read n topics or 1 ring topic
- each node knows the map of nodes that should be active and reachable
  - how publish the map? is the map hardcoded? do we asume the start of a first leader/orchestartor?
# notes - Distributed_Systems_4
- 5.4 Election algorithms
  - the bully algorithm
    When any process notices that the coordinator is no longer responding to requests, it initiates an election. A process, Pk, holds an election as follows:
    1. Pk sends an ELECTION message to all processes with higher identifiers: Pk+1, Pk+2, . . . , PN−1.
    2. If no one responds, Pk wins the election and becomes coordinator.
    3. If one of the higher-ups answers, it takes over and Pk’s job is done.
    At any moment, a process can get an ELECTION message from one of its lower-numbered colleagues. When such a message arrives, the receiver sends an OK message back to the sender to indicate that it is alive and will take over. The receiver then holds an election, unless it is already holding one. Eventually, all processes give up but one, and that one is the new coordinator.
    If a process that was previously down comes back up, it holds an election.

  - a ring algorithm
    Unlike some ring algorithms, this one does not use a token.
    We assume that each process knows who its successor is.
    When any process notices that the coordinator is not functioning, it builds an ELECTION message containing its own process identifier and sends the message to its successor.
    If the successor is down, the sender skips over the successor and goes to the next member along the ring, or the one after that, until a running process is located.
    At each step along the way, the sender adds its own identifier to the list in the message, effectively making itself a candidate to be elected as coordinator.
    Eventually, the message gets back to the process that started it all. That process recognizes this event when it receives an incoming message containing its own identifier. At that point, the message type is changed to COORDINATOR and circulated once again, this time to inform everyone else who the coordinator is (the list member with the highest identifier) and who the members of the new ring are. When this message has circulated once, it is removed and everyone goes back to work.
    
  - Leader election in ZooKeeper
    ZooKeeper is logically centralized coordination service.
    ZooKeeper maintains a (relatively small) set of servers, forming what is called an ensemble. For a client, an ensemble appears as just a single server. For ZooKeeper, this ensemble is also coordinated by a single server, called the leader.
    The other servers are called followers and essentially act as up-to-date standbys for whenever the leader malfunctions.
    When a follower s believes something is wrong with the leader (e.g., it suspects that the leader crashed), it sends out an ELECTION message to all other servers, along with the pair (voteID,voteTX). For this first message, it sets voteID to id(s) and voteTX to tx(s). During an election, each server s maintains two variables. The first, leader(s), records the identifier of the server that s believes may turn out to be the final leader, and is initialized to id(s).
    The second, lastTX(s) is what s has learned to be the most recent transaction, initially being its own value, namely tx(s).
    When a server s∗ receives (voteID,voteTX), it proceeds as follows:
      • If lastTX(s∗) < voteTX, then s∗ just received more up-to-date information on the most recent transaction. In that case it sets
        – leader(s∗) ← voteID
        – lastTX(s∗) ← voteTX
      • If lastTX(s∗) = voteTX and leader(s∗) < voteID, then s∗ knows as much about the most recent transaction as what it was just sent, but its perspective on which server will be the next leader needs to be updated:
        – leader(s∗) ← voteID
    Each time a server s∗ receives a (voteID,voteTX) message, it may update its own information on whom it suspects to be the next leader. If s∗ believes it should be the next leader, and has not sent out a message stating this, it will broadcast the pair (id(s∗),tx(s∗)). Under the assumption that communication is reliable, this broadcast alone should do the job.
    Coming to the conclusion that one of the servers in a ZooKeeper ensemble is now indeed the new leader, can be a bit tricky. One way is to let a server come to the conclusion that it may never become a leader in the current round, in which case it tells the alleged leader that it will become a follower. This means that as soon as a server has collected enough followers, it can promote itself to leader.

  - Leader election in Raft
    
- 8
