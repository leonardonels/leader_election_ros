# assumptions:
- each node knows its PID
- each node publishes its PID
  - n topics vs 1 topic with ring/append message
- each node can read n topics or 1 ring topic
- each node knows the map of nodes that should be active and reachable
  - how publish the map? is the map hardcoded? do we asume the start of a first leader/orchestartor?
