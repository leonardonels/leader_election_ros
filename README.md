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
