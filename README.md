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

  - Leader election in Raft
    
- 8
