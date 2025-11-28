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
    Raft operates in a setting in which a handful of known replicated servers (typically five) collaborate by ensuring that each server executes the same set of operations, and all in the same order.
    To this end, one of the servers is elected as leader to tell the others what the exact order is of those operations.
    The protocol assumes that messages may be lost and that servers may crash.
    Each server can be in one of three states: follower, candidate, or leader.
    Furthermore, the protocol operates in terms, where during each term there is exactly one leader, although it could have perhaps crashed. Each term is numbered, starting with 0. 
    The leader is assumed to regularly send out a message, either containing information on an operation that should be carried out, or otherwise a heartbeat message to tell the other servers that their leader is still up and running.
    Each server initially starts in the follower state (that is, there is initially no leader). After a follower timeout, a following server concludes that the leader may have crashed (which, by the way, may be a false conclusion). As a result, it enters the candidate state and starts an election, volunteering to be the new leader. An election starts with a broadcast to all other servers, along with increasing the term number by 1. At that point, three situations may happen.
      1. A candidate may receive a message from an alleged leader. If that server indicates it is operating in the same term as the candidate server, the latter will become follower again for the current term.
      2. When a following server receives an election message for the first time (in a new term), it simply votes for the candidate and ignores any other election messages (for that new term). Therefore, a candidate server can also receive a vote. If it has a majority of votes (i.e., more than half of the servers, including itself), it promotes itself as leader for the new term.
      3. As long as there is no alleged leader, or not enough votes have been received, the candidate server waits until a candidate timeout happens. At that point, the candidate server will simply start a new election (and again, for a next term).
    Of course, if all servers enter the candidate state, we bear the risk of indefinitely not being able to cast enough votes for a single server. A simple solution is to slightly vary the follower timeout and candidate timeout on a per-server basis. The result is that, generally, there is only a single candidate for the new term, allowing each other server to vote for that candidate and become a follower. As soon as a server becomes leader, it will send out a heartbeat message to the rest.

  - Elections in wireless environments
    Traditional election algorithms are generally based on assumptions that are not realistic in wireless environments. For example, they assume that message passing is reliable and that the topology of the network does not change. These assumptions are false in most wireless environments, especially when dealing with mobile devices.
    But in a ROS environment could remove the assumption that the network structure is known.
    Otherwise, if we assume that the configuration is knwon, probably the best option is to have the first lear fixed and be him to send the configuration to the othres. In that case we also talk about aggregation? Since ROS has broadcast topics no, simply each follower should subscribe to the leader's config topic that is publishing.

- 8.1 fault tolerance
  - Being fault tolerant is strongly related to what are called dependable systems. Dependability is a term that covers a number of useful requirements for distributed systems:
      • Availability
      • Reliability
      • Safety
      • Maintainability
    Availability is defined as the property that a system is ready to be used immediately. In general, it refers to the probability that the system is operating correctly at any given moment, and is available to perform its functions on behalf of its users. In other words, a highly available system is one that will most likely be working at a given instant in time. 
    Reliability refers to the property that a system can run continuously without failure. In contrast to availability, reliability is defined in terms of a time interval, instead of an instant in time. A highly reliable system is one that will most likely continue to work without interruption during a relatively long period of time. This is a subtle but important difference when compared to availability. If a system goes down on average for one, seemingly random millisecond every hour, it has an availability of more than 99.9999 percent, but is still unreliable. Similarly, a system that never crashes but is shut down for two specific weeks every August, has high reliability but only 96 percent availability. The two are not the same. 
    Safety refers to the situation that when a system temporarily fails to operate correctly, no catastrophic event happens. For example, many process- control systems, such as those used for controlling nuclear power plants or sending people into space, are required to provide a high degree of safety. If such control systems temporarily fail for only a very brief moment, the effects could be disastrous. Many examples from the past (and probably many more yet to come) show how hard it is to build safe systems. 
    Finally, maintainability refers to how easily a failed system can be repaired. A highly maintainable system may also show a high degree of availability, especially if failures can be detected and repaired automatically. However, as we shall see later in this chapter, automatically recovering from failures is easier said than done.

  - To get a better grasp on how serious a failure actually is, several classification schemes have been developed.
    A crash failure occurs when a server prematurely halts, but was working correctly until it stopped. An important aspect of crash failures is that once the server has halted, nothing is heard from it anymore. A typical example of a crash failure is an operating system that comes to a grinding halt, and for which there is only one solution: reboot it. Many personal computer systems (be they desktop computers or laptops) suffer from crash failures so often that people have come to expect them to be normal. Consequently, moving the reset button, for example, from the back of a cabinet to the front was done for good reason. Perhaps one day it can be moved to the back again, or even removed altogether. 
    An omission failure occurs when a server fails to respond to a request. Several things might go wrong. In the case of a receiveomission failure, possibly the server never got the request in the first place. Note that it may well be the case that the connection between a client and a server has been correctly established, but that there was no thread listening for incoming requests. Also, a receive-omission failure will generally not affect the current state of the server, as the server is unaware of any message sent to it. 
    Likewise, a send-omission failure happens when the server has done its work, but somehow fails in sending a response. Such a failure may happen, for example, when a send buffer overflows while the server was not prepared for such a situation. Note that, in contrast to a receive-omission failure, the server may now be in a state reflecting that it has just completed a service for the client. As a consequence, if the sending of its response fails, the server has to be prepared for the client to reissue its previous request. 
    Other types of omission failures not related to communication may be caused by software errors such as infinite loops or improper memory management, by which the server is said to “hang.” 
    Another class of failures is related to timing. Timing failures occur when the response lies outside a specified real-time interval. For example, in the case of streaming videos, providing data too soon may easily cause trouble for a recipient if there is not enough buffer space to hold all the incoming data. More common, however, is that a server responds too late, in which case a performance failure is said to occur. 
    A serious type of failure is a response failure, by which the server’s response is simply incorrect. Two kinds of response failures may happen. In the case of a value failure, a server simply provides the wrong reply to a request. For example, a search engine that systematically returns Web pages not related to any of the search terms used, has failed. 
    The other type of response failure is known as a state-transition failure. This kind of failure happens when the server reacts unexpectedly to an incoming request. For example, if a server receives a message it cannot recognize, a state-transition failure happens if no measures have been taken to handle such messages. In particular, a faulty server may incorrectly take default actions it should never have initiated. 
    The most serious are arbitrary failures, also known as Byzantine failures. In effect, when arbitrary failures occur, clients should be prepared for the worst. In particular, a server may be producing output it should never have produced, but which cannot be detected as being incorrect.

  - With physical redundancy, extra equipment or processes are added to make it possible for the system as a whole to tolerate the loss or malfunctioning of some components. Physical redundancy can thus be done either in hardware or in software. For example, extra processes can be added to the system, so that if a few of them crash, the system can still function correctly. In other words, by replicating processes, a high degree of fault tolerance may be achieved
    Maybe placing an entire logic before each ROS node that elects a leader and wait for it to decide which node to activate. Only node that th eleader decides will spinn the actual node. This way you could have multiple vesrion of the same node ready to be spinned like a faster and readier backup.
  - Raft
    In Raft, we typically have a group of some five replicated servers. We assume the set of servers is fixed (although Raft allows servers to join and leave the group). Each server maintains a log of operations, some of which have already been executed (i.e., committed), as well as pending operations. Consensus is expressed in terms of these logs: committed operations have the same position in each of the respective server’s logs. One of the servers operates as a leader and decides on the order in which pending operations are to be committed. In essence, Raft is a primary-backup protocol, with the primary acting as leader and the backups as followers.
    A client always sends an operation request to the leader (possibly after having been redirected by one of the followers). That means that the leader is fully aware of all pending requests. Each client request for executing an operation o is appended to the leader’s log, in the form of a tuple ⟨o, t, k⟩ in which t is the term under which the current leader serves, and k the index of o in the leader’s log. To recall, after electing a next leader, the term for new operations will be t + 1. Let c be the index of the most recently committed operation. Raft guarantees that operations that have been registered as committed, have been performed by a majority of the servers, and that the result has been returned to the original client.
  - Paxos
    The assumptions under which Paxos operates are rather weak:
      • The distributed system is partially synchronous (in fact, it may even be asynchronous).
      • Communication between processes may be unreliable, meaning that messages may be lost, duplicated, or reordered.
      • Messages that are corrupted can be detected as such (and thus subsequently ignored).
      • All operations are deterministic: once an execution is started, it is known exactly what it will do.
      • Processes may exhibit crash failures, but not arbitrary failures, nor do processes collude.
      ...

  The issue is that all of this algorithms are originally designed for server-client connections, but what this project is tring to achieve is to make crash resiliat application in the robotic domain in which there are multiple broadcasting nodes with the same priority.
