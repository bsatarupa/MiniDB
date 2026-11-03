# Raft Consensus

## Overview

Raft is a distributed consensus algorithm designed to achieve agreement among multiple nodes on a replicated sequence of operations.

It enables a distributed system to maintain a consistent state even when individual nodes crash, restart, or temporarily lose network connectivity.

Raft uses a leader-based architecture. At any point in time, a cluster attempts to maintain a single elected leader responsible for coordinating log replication and establishing the order of operations.

Other nodes operate as followers and replicate the leader's log. If the leader becomes unavailable, followers initiate an election to select a new leader.

MiniDB will use Raft as the consensus protocol for coordinating replicated database operations across multiple nodes.

The fundamental abstraction is a replicated log. Every node maintains a local copy of the log, and Raft ensures that committed entries are consistently ordered across the cluster.

The database state machine applies committed log entries in order to produce equivalent logical database states on every replica.

### Core Concepts

| Concept | Description |
|---|---|
| Cluster | Collection of nodes participating in consensus |
| Node | Individual MiniDB instance participating in Raft |
| Leader | Node responsible for coordinating log replication |
| Follower | Node that receives and persists replicated entries |
| Candidate | Node attempting to become the leader |
| Term | Logical election epoch |
| Raft Log | Ordered sequence of replicated operations |
| Log Index | Position of an entry in the replicated log |
| Commit Index | Highest log index known to be committed |
| Last Applied | Highest log index applied to the state machine |
| Quorum | Minimum number of nodes required for consensus |
| Majority | More than half of the cluster's voting members |
| Heartbeat | Periodic communication from the leader to followers |
| Snapshot | Compacted representation of the replicated state |
| Membership Change | Addition or removal of cluster members |

### Raft Architecture

    +-----------------------------------------------------------+
    |                     MiniDB Cluster                        |
    |                                                           |
    |       +----------------------+                            |
    |       |        Leader        |                            |
    |       |                      |                            |
    |       |  Raft Log            |                            |
    |       |  WAL                 |                            |
    |       |  Commit Index        |                            |
    |       +----------------------+                            |
    |                  |                                        |
    |          +-------+-------+                                |
    |          |               |                                |
    |          v               v                                |
    |    +-----------+   +-----------+                          |
    |    | Follower  |   | Follower  |                          |
    |    |           |   |           |                          |
    |    | Raft Log  |   | Raft Log  |                          |
    |    | WAL       |   | WAL       |                          |
    |    +-----------+   +-----------+                          |
    |                                                           |
    +-----------------------------------------------------------+

Example three-node cluster:

    Node A: Leader
    Node B: Follower
    Node C: Follower

    Majority = 2 nodes

### Raft RPCs

Raft primarily uses three RPC mechanisms:

| RPC | Purpose |
|---|---|
| RequestVote | Requests votes during leader election |
| AppendEntries | Replicates log entries and sends heartbeats |
| InstallSnapshot | Transfers snapshots to followers that are too far behind |

The first two RPCs are fundamental to the initial Raft implementation. Snapshot transfer is introduced when log compaction and replica recovery are implemented.

### Raft Safety Properties

The protocol must preserve the following properties:

**Election Safety**

At most one leader can be elected in a given term.

**Leader Append-Only**

A leader never overwrites or deletes entries in its own log. It only appends new entries.

**Log Matching**

If two logs contain an entry with the same index and term, their preceding entries are identical.

**Leader Completeness**

If an entry is committed in a given term, that entry is present in the logs of every leader elected in subsequent terms.

**State Machine Safety**

If a node applies a log entry at a particular index, no other node can apply a different entry at that index.

These properties ensure that committed operations remain consistent across replicas.

---

## Goals

1. Implement a leader-based consensus protocol for MiniDB.
2. Maintain a consistent replicated log across cluster nodes.
3. Support automatic leader election.
4. Detect leader failures through heartbeat timeouts.
5. Replicate log entries using AppendEntries RPC.
6. Persist Raft state and log entries across restarts.
7. Prevent multiple leaders from independently committing operations.
8. Ensure committed entries are never overwritten.
9. Recover followers with missing or conflicting log entries.
10. Support safe cluster membership changes.
11. Handle node crashes and network partitions.
12. Support snapshot-based log compaction.
13. Separate consensus logic from database storage implementation.
14. Integrate with WAL, checkpointing, and database recovery.
15. Provide a foundation for highly available distributed database deployments.

### Non-Goals for Initial Implementation

- Multi-leader consensus.
- Byzantine fault tolerance.
- Active-active write replication.
- Cross-region consensus optimization.
- Automatic database sharding.
- Distributed transaction coordination.
- Multi-Raft groups.
- Geo-distributed quorum placement.

### Initial Cluster Configuration

The initial implementation targets three voting nodes.

| Property | Configuration |
|---|---|
| Cluster size | 3 nodes |
| Leader count | 1 |
| Followers | 2 |
| Quorum | 2 |
| Failure tolerance | 1 node |
| Consensus algorithm | Raft |
| Log replication | Leader-based |
| Election | Majority voting |
| Persistence | Local durable log |
| Membership | Static initially |

For a cluster with N voting members:

    quorum = floor(N / 2) + 1

| Cluster Size | Quorum | Tolerated Node Failures |
|---:|---:|---:|
| 1 | 1 | 0 |
| 3 | 2 | 1 |
| 5 | 3 | 2 |
| 7 | 4 | 3 |

A cluster can commit new entries only while a majority of its voting members can communicate and satisfy the required persistence conditions.

---

## Node States

Every Raft node operates in one of three states:

- Leader
- Follower
- Candidate

A node can transition between these states as elections occur and terms change.

### Node State Diagram

    +--------------------+
    |      Follower      |
    +--------------------+
          |         ^
          |         |
    Election        | Higher Term
    Timeout         |
          |         |
          v         |
    +--------------------+
    |     Candidate      |
    +--------------------+
          |         |
    Majority   Higher Term
    Obtained       |
          |         |
          v         |
    +--------------------+
    |       Leader       |
    +--------------------+
          |
          | Higher Term
          +-----------------------> Follower

### Persistent and Volatile State

Raft maintains state that must survive restarts and state that can be reconstructed.

| State | Description | Persistence |
|---|---|---|
| `current_term` | Latest term observed | Persistent |
| `voted_for` | Candidate voted for in current term | Persistent |
| `log` | Replicated log entries | Persistent |
| `commit_index` | Highest committed index | Volatile |
| `last_applied` | Highest applied index | Volatile or recoverable |
| `next_index[]` | Next entry to send to each follower | Leader volatile state |
| `match_index[]` | Highest replicated index per follower | Leader volatile state |

Persistent Raft state must be written durably before the node responds in a way that relies on that state.

In particular:

- Updated terms must be persisted.
- Votes must be persisted before granting them.
- Accepted log entries must be persisted before acknowledging their successful replication.

### Conceptual Node State

    enum class RaftRole {
        FOLLOWER,
        CANDIDATE,
        LEADER
    };

    struct RaftPersistentState {
        uint64_t current_term;
        std::optional<NodeId> voted_for;
    };

    struct RaftVolatileState {
        RaftRole role;

        uint64_t commit_index;
        uint64_t last_applied;
    };

The persistent Raft log is managed separately.

### Leader

The leader is responsible for coordinating the cluster's replicated log.

Only one leader should be elected in a given term.

Responsibilities:

- Accept client write requests.
- Assign log indexes.
- Append operations to its local log.
- Persist log entries.
- Replicate entries to followers.
- Track replication progress.
- Determine when entries become committed.
- Advance the commit index.
- Apply committed entries to the state machine.
- Send periodic heartbeats.
- Detect follower failures.
- Coordinate snapshot transfer.
- Respond to clients according to the configured consistency and durability guarantees.

#### Leader-Specific State

The leader maintains replication progress for every follower.

    struct FollowerProgress {
        NodeId node_id;

        uint64_t next_index;
        uint64_t match_index;
    };

    struct LeaderState {
        std::unordered_map<NodeId, FollowerProgress> followers;
    };

`next_index` is the next log index the leader intends to send to a follower.

`match_index` is the highest log index known to be replicated on that follower.

#### Leader Initialization

When a node becomes leader:

1. Initialize `next_index` for every follower to the leader's last log index plus one.
2. Initialize `match_index` for every follower to the known matching prefix.
3. Begin sending periodic heartbeats.
4. Append a no-op entry in the new term.
5. Start replicating pending log entries.
6. Advance the commit index when Raft's commitment conditions are satisfied.

The no-op entry allows the new leader to establish commitment in its own term and safely advance the commit index over preceding entries.

#### Leader Step-Down

A leader must transition to follower state when it discovers a higher term.

It must:

- Update its current term.
- Clear its previous vote as required for the new term.
- Stop accepting writes as the authoritative leader.
- Stop making commitment decisions for the old term.
- Begin following the newer leader.

### Follower

Followers receive log entries and heartbeats from the current leader.

They participate in elections and maintain a local replicated log.

Responsibilities:

- Process RequestVote RPCs.
- Process AppendEntries RPCs.
- Validate log prefixes.
- Persist accepted log entries.
- Acknowledge successful replication.
- Update the commit index based on leader information.
- Apply committed entries in order.
- Monitor leader heartbeats.
- Start elections when the leader becomes unavailable.
- Receive snapshots when required.

Followers do not independently commit client writes.

#### Follower Behavior

A follower remains in this state while it receives valid leader communication.

When it receives an AppendEntries request:

1. Compare the request term with its current term.
2. Update its term if the request contains a higher term.
3. Reset its election timeout.
4. Verify the previous log index and term.
5. Reject the request if the log prefix does not match.
6. Resolve conflicts in the uncommitted suffix.
7. Append new entries.
8. Persist accepted entries.
9. Update its commit index.
10. Apply newly committed entries in order.

If the follower's election timeout expires without valid leader communication, it transitions to candidate state.

### Candidate

A candidate attempts to become the leader.

A follower transitions to candidate when its election timeout expires.

Responsibilities:

- Increment the current term.
- Vote for itself.
- Persist its vote.
- Request votes from other nodes.
- Collect election responses.
- Become leader after receiving a majority.
- Return to follower state if it discovers a higher term.

#### Candidate Transition

    Follower
       |
       | Election Timeout
       v
    Increment Current Term
       |
       v
    Vote for Self
       |
       v
    Persist Term and Vote
       |
       v
    Send RequestVote RPC
       |
       v
    Collect Votes
       |
       v
    Majority Obtained?
       |
       +------ Yes ------> Leader
       |
       +------ No -------> Retry Election

If a candidate receives an AppendEntries RPC from a legitimate leader with an equal or higher term, it transitions to follower state.

If a candidate receives a higher-term RequestVote or other valid higher-term RPC, it also updates its term and becomes a follower.

---

## Leader Election

Leader election ensures that the cluster has a single authoritative node responsible for coordinating log replication.

Raft uses terms and majority voting to elect leaders.

### Terms

A term represents a logical election epoch.

Terms increase monotonically.

    Term 1: Node A elected leader.
    Term 2: Node B elected leader.
    Term 3: Node C elected leader.

Each node stores its current term persistently.

A node that receives a request containing a higher term must update its current term and transition to follower state.

A node must reject requests from older terms.

### Election Timeout

Followers monitor leader availability using an election timeout.

If a follower does not receive valid AppendEntries communication before the timeout expires, it starts an election.

Election timeouts should be randomized to reduce simultaneous elections.

Illustrative initial configuration:

    Heartbeat Interval:       100 ms
    Minimum Election Timeout: 300 ms
    Maximum Election Timeout: 600 ms

These values are starting points for testing, not production recommendations.

The actual values must account for:

- Network latency.
- Disk persistence latency.
- Scheduler delays.
- CPU contention.
- Deployment environment.

### RequestVote RPC

Candidates use RequestVote RPCs to request votes from other nodes.

Conceptual request:

    struct RequestVoteRequest {
        uint64_t term;
        NodeId candidate_id;

        uint64_t last_log_index;
        uint64_t last_log_term;
    };

Conceptual response:

    struct RequestVoteResponse {
        uint64_t term;
        bool vote_granted;
    };

### Vote Granting Rules

A follower grants its vote only if:

1. The candidate's term is not older than the follower's current term.
2. The follower has not voted for another candidate in the same term, or has already voted for this candidate.
3. The candidate's log is at least as up-to-date as the follower's log.

The log freshness comparison uses:

1. Last log term.
2. Last log index when last log terms are equal.

A candidate with a newer last log term is considered more up-to-date.

If both last log terms are equal, the candidate with the longer log is more up-to-date.

### Election Workflow

    +---------------------+
    |      Follower       |
    +---------------------+
              |
              | Election Timeout
              v
    +---------------------+
    |     Candidate       |
    |                     |
    | Increment Term      |
    | Vote for Self       |
    | Persist Vote        |
    +---------------------+
              |
              | RequestVote RPC
              v
    +---------------------+
    | Request Votes       |
    | from Other Nodes    |
    +---------------------+
              |
              v
    +---------------------+
    | Majority Obtained?  |
    +---------------------+
          |         |
         Yes        No
          |         |
          v         v
    +-----------+  Retry
    |  Leader   |  Election
    +-----------+

### Election Safety

A node can grant at most one vote per term.

Because a majority of any two majorities must overlap, two different candidates cannot both receive a majority in the same term.

This establishes the Election Safety property.

### Election Retry

If a candidate fails to obtain a majority:

1. It waits for another randomized election timeout.
2. It increments its term.
3. It votes for itself.
4. It requests votes again.

A pre-vote mechanism may be introduced later to reduce unnecessary term increases caused by isolated or recovering nodes.

---

## Heartbeats

Heartbeats allow the leader to inform followers that it is still active.

Raft uses AppendEntries RPCs for heartbeats.

A heartbeat is an AppendEntries request containing no new log entries.

### Heartbeat Responsibilities

- Maintain leader authority.
- Prevent unnecessary elections.
- Communicate the current term.
- Communicate the leader's commit index.
- Detect unavailable followers.
- Maintain replication coordination.

### Heartbeat Workflow

    +---------------------+
    |       Leader        |
    +---------------------+
              |
              | Periodic Timer
              v
    +---------------------+
    | Send AppendEntries  |
    | with Empty Entries  |
    +---------------------+
              |
              v
    +---------------------+
    |      Followers      |
    +---------------------+
              |
              v
    +---------------------+
    | Validate Term       |
    | Reset Election     |
    | Timeout             |
    | Update Commit Index |
    +---------------------+

### Heartbeat Interval

The heartbeat interval should be significantly shorter than the election timeout.

For example:

    Heartbeat Interval:       100 ms
    Election Timeout:         300-600 ms

The relationship should be configurable.

### Heartbeat Failure

If a follower does not receive valid heartbeats or other valid leader communication before its election timeout expires, it may start an election.

If the leader stops receiving acknowledgments from a follower:

- It should mark the follower as potentially unavailable.
- It should continue replicating to other followers.
- It should continue committing writes only if a quorum remains available.
- It should retry replication when the follower becomes reachable.

### Heartbeats and Commit Index

Heartbeats also communicate the leader's current commit index.

A follower can advance its commit index using the leader's advertised commit index, bounded by the last index present in its own log.

Conceptually:

    follower.commit_index =
        min(
            leader_commit,
            follower.last_log_index
        );

A follower then applies committed entries that have not yet been applied.

---

## Log Replication

Log replication is the process through which the leader transfers ordered log entries to followers.

The replicated log is the foundation of Raft consensus.

Each entry contains:

- Log index.
- Term in which the entry was created.
- Operation type.
- Serialized payload.

### Raft Log Entry

Conceptual representation:

    enum class RaftEntryType {
        NO_OP,
        DATA_OPERATION,
        CONFIGURATION_CHANGE
    };

    struct RaftLogEntry {
        uint64_t index;
        uint64_t term;

        RaftEntryType type;

        std::vector<std::byte> payload;
    };

The actual on-disk format must use explicit serialization.

Raw C++ struct layouts must not be written directly to persistent storage.

### Log Structure

    +------------------------------------------------+
    |                  Raft Log                      |
    +------------------------------------------------+
    | Index | Term | Operation                       |
    +------------------------------------------------+
    |   1   |  1   | INSERT key=100                  |
    |   2   |  1   | UPDATE key=100                  |
    |   3   |  2   | INSERT key=200                  |
    |   4   |  2   | DELETE key=100                  |
    +------------------------------------------------+

Every entry has a unique log index within the log history.

The term identifies the election epoch in which the entry was created.

### AppendEntries RPC

The leader uses AppendEntries RPC to replicate entries.

Conceptual request:

    struct AppendEntriesRequest {
        uint64_t term;
        NodeId leader_id;

        uint64_t prev_log_index;
        uint64_t prev_log_term;

        std::vector<RaftLogEntry> entries;

        uint64_t leader_commit;
    };

Conceptual response:

    struct AppendEntriesResponse {
        uint64_t term;
        bool success;

        uint64_t match_index;
    };

### Leader Replication Workflow

1. Receive a client write request.
2. Serialize the operation.
3. Assign the next log index.
4. Attach the current term.
5. Append the entry to the leader's local log.
6. Persist the entry.
7. Send AppendEntries RPCs to followers.
8. Followers validate the previous log index and term.
9. Followers append and persist accepted entries.
10. Followers acknowledge successful replication.
11. The leader updates follower replication progress.
12. The leader advances the commit index when quorum conditions are satisfied.
13. The state machine applies committed entries in order.

### Log Matching

The leader includes the previous log index and term in each AppendEntries request.

The follower verifies that its log contains a matching entry.

If the previous entry does not match:

- The follower rejects the request.
- The leader adjusts the follower's `next_index`.
- The leader retries with an earlier log position.

Once a matching prefix is found, the follower can accept the remaining entries.

### Conflict Resolution

If a follower contains conflicting uncommitted entries:

1. Identify the first conflicting index.
2. Remove the conflicting uncommitted suffix.
3. Append the leader's entries.
4. Persist the updated log.
5. Acknowledge successful replication.

A follower must never remove or overwrite a committed entry.

### Commit Index

The commit index identifies the highest log index known to be committed.

The leader advances its commit index when:

1. An entry is replicated on a majority of the cluster.
2. The entry belongs to the leader's current term.

Once a current-term entry is committed, preceding entries in the log become committed as well.

A leader must not commit an older-term entry solely by counting replicas.

### Example

    Leader Log:

    Index:  1    2    3    4    5
    Term:   1    1    2    2    2

    Node A: 1    2    3    4    5
    Node B: 1    2    3    4
    Node C: 1    2    3

    Majority replicated through index 4.

    If index 4 belongs to the current term,
    the leader can advance commit_index to 4.

### Applying Committed Entries

The Raft log and state machine have separate responsibilities.

The Raft log determines which operations are committed.

The state machine applies committed operations to the database.

    Raft Log
       |
       v
    Commit Index
       |
       v
    Apply Entries in Order
       |
       v
    Database State Machine
       |
       v
    MiniDB Storage Engine

Every node applies committed entries in log-index order.

The `last_applied` index tracks the last entry applied by the local state machine.

The implementation must ensure that:

    last_applied <= commit_index

### WAL Integration

The Raft log must be durable.

A node must persist accepted log entries before acknowledging their replication.

The implementation must define the relationship between:

- Raft log persistence.
- Database WAL persistence.
- Commit-index advancement.
- State-machine application.
- Database checkpoints.

The initial design should maintain clear logical boundaries between consensus logging and database recovery.

### State Machine Determinism

All replicas must produce equivalent logical states after applying the same committed entries.

Replicated operations must therefore be deterministic.

Potential sources of nondeterminism include:

- Local timestamps.
- Random values.
- Unordered data-structure iteration.
- Independent sequence-number allocation.
- Node-specific configuration.
- Environment-dependent operation behavior.

Values that must be consistent across replicas should be established by the leader and included in the replicated payload, or otherwise derived deterministically.

---

## Membership Changes

Membership changes allow nodes to be added to or removed from a Raft cluster.

Changing the voting membership is a consensus-sensitive operation because it changes the quorum required to commit entries and elect leaders.

MiniDB will initially use a static cluster configuration.

Dynamic membership changes are a future extension.

### Why Membership Changes Are Difficult

Consider a cluster changing from three nodes to five nodes.

Old configuration:

    A, B, C

    Quorum = 2

New configuration:

    A, B, C, D, E

    Quorum = 3

If different nodes independently use different configurations, they may make conflicting consensus decisions.

A safe transition must coordinate the old and new configurations.

### Joint Consensus

Raft supports membership changes through joint consensus.

During the transition, the cluster uses an intermediate configuration containing both the old and new memberships.

    Old Configuration:
        A, B, C

              |
              v

    Joint Configuration:
        A, B, C, D, E

              |
              v

    New Configuration:
        A, B, C, D, E

During the joint configuration, decisions require majorities from both the old and new configurations.

This prevents incompatible quorum decisions during the transition.

### Adding a Node

A future node-addition workflow:

1. Provision the new MiniDB instance.
2. Assign a unique node ID.
3. Establish network connectivity.
4. Add the node as a non-voting learner.
5. Replicate existing log entries.
6. Bring the learner sufficiently up to date.
7. Begin a joint consensus configuration change.
8. Obtain the required majorities.
9. Commit the new configuration.
10. Promote the node to a voting member.

The learner stage reduces the risk of adding a voting member that does not yet possess the required log history.

### Removing a Node

A future node-removal workflow:

1. Identify the node to remove.
2. Verify that the remaining configuration can maintain a quorum.
3. Propose a configuration change.
4. Enter joint consensus.
5. Obtain the required majorities.
6. Commit the new configuration.
7. Remove the node from the active membership.
8. Update cluster metadata.

### Configuration Entries

Membership changes should be represented as replicated configuration entries.

Conceptual representation:

    struct ClusterConfiguration {
        uint64_t configuration_index;

        std::vector<NodeId> old_members;
        std::vector<NodeId> new_members;
    };

The persistent representation must also include the relevant membership-change state.

### Initial Scope

The first implementation will support:

- Static node membership.
- Fixed cluster configuration.
- Configured node identities.
- Configured peer endpoints.
- Cluster restart using the same configuration.

Joint consensus, learner nodes, and dynamic membership are deferred.

---

## Failure Handling

Raft must continue to preserve safety when nodes crash, restart, or lose communication.

Failure handling must distinguish between temporary unavailability and permanent data loss.

### Leader Failure

Consider a three-node cluster:

    Before Failure:

        Node A: Leader
        Node B: Follower
        Node C: Follower

    Node A becomes unavailable.

    Followers detect the election timeout and start an election.

    After Election:

        Node A: Unavailable
        Node B: Leader
        Node C: Follower

The cluster can continue committing writes because B and C form a majority.

### Follower Failure

If a follower becomes unavailable:

- The leader continues operating if a majority remains available.
- The unavailable follower stops receiving log entries.
- The leader continues tracking the follower's replication progress.
- The follower catches up after reconnecting.

### Network Partition

Consider a three-node cluster split into two partitions.

    Partition A:             Partition B:

    Node A (Leader)          Node B (Follower)
                             Node C (Follower)

The majority partition contains B and C.

The minority partition contains A.

The majority partition can elect a new leader and continue committing writes.

The isolated old leader cannot commit new entries because it cannot obtain a majority.

When connectivity returns:

1. The old leader receives a higher term.
2. It transitions to follower state.
3. Its conflicting uncommitted entries are reconciled.
4. The new leader's committed history is preserved.

### Process Restart

When a node restarts:

1. Load persistent Raft metadata.
2. Recover the current term.
3. Recover the persisted vote.
4. Scan the local Raft log.
5. Validate record lengths and checksums.
6. Identify incomplete or corrupt trailing records.
7. Recover the last valid log index and term.
8. Recover or reconstruct the database state-machine position.
9. Start as a follower.
10. Reconnect to the current leader.
11. Reconcile the local log.
12. Resume replication.

A node must not assume that all locally persisted entries are committed.

### Uncommitted Entries

Uncommitted entries may be removed if they conflict with the log of a newly elected leader.

Committed entries must never be removed.

The leader's election and log-matching restrictions ensure that committed entries are preserved across leadership changes.

### Snapshot-Based Recovery

If a follower is too far behind and the leader no longer retains the required log prefix, the leader must transfer a snapshot.

A snapshot represents the state machine after applying entries up to a particular log index.

Conceptual metadata:

    struct SnapshotMetadata {
        uint64_t last_included_index;
        uint64_t last_included_term;

        uint64_t configuration_index;
    };

The snapshot must be installed safely and atomically.

A partially received snapshot must not replace a valid existing database state.

### Majority Loss

If a majority of the cluster becomes unavailable:

- The cluster cannot elect a new leader.
- The cluster cannot commit new writes.
- Existing committed data remains available on surviving nodes, subject to their local storage integrity.
- Normal write availability resumes only after a majority is restored or an explicitly controlled recovery procedure is performed.

Raft prioritizes consistency over write availability during a majority partition.

### Failure Scenarios

| Failure | Expected Behavior |
|---|---|
| Leader process crash | Followers elect a new leader |
| Follower process crash | Leader continues if quorum remains |
| Leader network isolation | Majority elects a new leader |
| Minority partition | Cannot commit new entries |
| Follower restart | Reconciles its log with leader |
| Stale candidate | Steps down after observing a higher term |
| Conflicting uncommitted suffix | Replaced during log reconciliation |
| WAL corruption | Recovery must detect corruption and fail safely |
| Majority loss | New writes unavailable |
| Snapshot transfer interruption | Existing valid state remains intact |

---

## Safety Guarantees

Raft is designed to maintain safety even when the cluster experiences failures.

Safety means that the system never violates its fundamental consistency rules.

### Election Safety

At most one leader can be elected in a term.

Every node grants at most one vote per term.

Because two majorities must overlap, two different candidates cannot both obtain a majority in the same term.

### Leader Append-Only

A leader never modifies or deletes entries in its own log.

It only appends new entries.

### Log Matching

If two logs contain an entry with the same index and term, all preceding entries must be identical.

The AppendEntries previous-index and previous-term checks establish this property.

### Leader Completeness

Every committed entry must be present in the log of every leader elected in a subsequent term.

The RequestVote log-freshness restriction prevents a candidate with an outdated log from being elected over a node holding a more up-to-date committed history.

### State Machine Safety

If one node applies a particular log entry at index N, no other node can apply a different entry at index N.

All committed entries are applied in log order.

### Majority Intersection

Any two majorities in a fixed cluster must share at least one node.

For example, in a three-node cluster:

    Majority 1: A, B
    Majority 2: B, C

    Shared Node: B

This intersection is fundamental to Raft's safety properties.

Membership changes require additional care because quorum configurations may differ. Joint consensus handles the transition between configurations.

### Committed Entries Are Not Lost

Once an entry is committed:

- It must remain in the replicated history.
- A newly elected leader must contain it.
- Followers must eventually receive it if they recover and remain part of the cluster.
- State machines must apply it in the correct order.

### Minority Partitions Cannot Commit

A node or group of nodes without a majority cannot commit new entries.

An isolated old leader cannot independently continue committing writes.

### Persistence Requirements

Raft safety depends on correct persistence behavior.

The implementation must guarantee:

1. Persistent term updates are durable.
2. Votes are persisted before being granted.
3. Accepted log entries are persisted before acknowledgment.
4. Acknowledgments accurately represent durable state.
5. Committed entries are never discarded.
6. Recovery reconstructs the correct log and state-machine position.

### Consensus Does Not Automatically Guarantee Linearizable Reads

Raft establishes a committed ordering of operations.

It does not automatically make every local database read linearizable.

For linearizable reads, MiniDB must establish that the serving leader still has valid authority and that the local state machine has applied the required committed entries.

A future implementation should use a quorum-based ReadIndex mechanism or a carefully designed leader lease with explicit timing assumptions.

Follower reads may be stale.

### Durability Assumptions

Raft protects against failures under its consensus and persistence assumptions.

It does not protect against every possible event.

Examples include:

- Permanent loss of all replicas holding committed data.
- Correlated disk failures.
- Undetected storage corruption.
- Incorrect durability acknowledgments.
- Bugs in log persistence or recovery.
- Byzantine behavior.

The initial MiniDB implementation assumes crash-stop or crash-recovery failures, non-Byzantine nodes, and durable local storage.

---

## Design Decisions

| Area | Initial Decision |
|---|---|
| Consensus protocol | Raft |
| Cluster architecture | Leader-follower |
| Node states | Leader, Follower, Candidate |
| Cluster size | 3 nodes |
| Voting model | Majority quorum |
| Term identifier | 64-bit unsigned integer |
| Log index | 64-bit unsigned integer |
| Leader election | Randomized election timeout |
| Vote RPC | RequestVote |
| Replication RPC | AppendEntries |
| Heartbeats | Empty AppendEntries |
| Log replication | Ordered replicated log |
| Commit rule | Majority replication with current-term restriction |
| State machine | Deterministic database operations |
| Persistent Raft state | Current term, voted-for, log |
| Follower recovery | Log reconciliation |
| Snapshot support | Deferred to later phase |
| Membership changes | Static initially |
| Joint consensus | Deferred |
| Learner nodes | Deferred |
| Multi-Raft | Deferred |
| Linearizable reads | ReadIndex-style mechanism planned |
| Byzantine tolerance | Not supported |

### Architectural Boundaries

Raft consensus should be independent of the physical database storage engine.

    +--------------------------------------------------+
    |                  Client Interface                |
    +--------------------------------------------------+
                          |
                          v
    +--------------------------------------------------+
    |                Replication Manager               |
    +--------------------------------------------------+
                          |
                          v
    +--------------------------------------------------+
    |                  Raft Node                       |
    |                                                  |
    | Election | Heartbeats | Log Replication          |
    +--------------------------------------------------+
                          |
                          v
    +--------------------------------------------------+
    |                    Raft Log                      |
    +--------------------------------------------------+
                          |
                          v
    +--------------------------------------------------+
    |                 WAL / Persistence                |
    +--------------------------------------------------+
                          |
                          v
    +--------------------------------------------------+
    |                  State Machine                   |
    +--------------------------------------------------+
                          |
                          v
    +--------------------------------------------------+
    |                MiniDB Storage Engine              |
    |                                                  |
    |          B+Tree | LSM | Buffer Pool               |
    +--------------------------------------------------+

**Raft Node**

Responsible for:

- Node state transitions.
- Term management.
- Election coordination.
- RequestVote processing.
- AppendEntries processing.
- Heartbeat handling.

**Raft Log**

Responsible for:

- Log entry creation.
- Log indexing.
- Log persistence.
- Log retrieval.
- Conflict resolution.
- Log truncation.
- Log compaction.

**Replication Manager**

Responsible for:

- Peer management.
- RPC communication.
- Replication scheduling.
- Follower progress tracking.
- Client write routing.
- Replication health.

**State Machine**

Responsible for:

- Applying committed entries.
- Maintaining applied-index metadata.
- Executing deterministic database operations.
- Coordinating with storage engines.

**WAL and Recovery**

Responsible for:

- Durable log records.
- Crash recovery.
- Checksum validation.
- Torn-write detection.
- Coordination with database checkpoints.

### Suggested Module Structure

    minidb/
    |
    +-- replication/
    |   |
    |   +-- raft_types.h
    |   |
    |   +-- raft_node.h
    |   +-- raft_node.cpp
    |   |
    |   +-- raft_state.h
    |   +-- raft_state.cpp
    |   |
    |   +-- raft_log.h
    |   +-- raft_log.cpp
    |   |
    |   +-- leader_election.h
    |   +-- leader_election.cpp
    |   |
    |   +-- heartbeat_manager.h
    |   +-- heartbeat_manager.cpp
    |   |
    |   +-- log_replication.h
    |   +-- log_replication.cpp
    |   |
    |   +-- membership_manager.h
    |   +-- membership_manager.cpp
    |   |
    |   +-- snapshot_manager.h
    |   +-- snapshot_manager.cpp
    |   |
    |   +-- state_machine.h
    |   +-- state_machine.cpp
    |   |
    |   +-- replication_manager.h
    |   +-- replication_manager.cpp
    |   |
    |   +-- peer_client.h
    |   +-- peer_client.cpp
    |
    +-- wal/
    |
    +-- recovery/
    |
    +-- storage/
        |
        +-- btree/
        +-- lsm/
        +-- buffer/

### Suggested Core Interfaces

These interfaces describe component responsibilities. They are conceptual and can evolve during implementation.

**Raft Node**

    class RaftNode {
    public:
        virtual void start() = 0;
        virtual void stop() = 0;

        virtual void handle_request_vote(
            const RequestVoteRequest& request
        ) = 0;

        virtual void handle_append_entries(
            const AppendEntriesRequest& request
        ) = 0;

        virtual void tick() = 0;

        virtual RaftRole role() const = 0;

        virtual uint64_t current_term() const = 0;

        virtual ~RaftNode() = default;
    };

**Raft Log**

    class RaftLog {
    public:
        virtual uint64_t last_log_index() const = 0;

        virtual uint64_t term_at(
            uint64_t index
        ) const = 0;

        virtual void append(
            const RaftLogEntry& entry
        ) = 0;

        virtual void truncate_suffix(
            uint64_t from_index
        ) = 0;

        virtual std::vector<RaftLogEntry> read_entries(
            uint64_t start_index,
            size_t max_entries
        ) const = 0;

        virtual void flush() = 0;

        virtual ~RaftLog() = default;
    };

**Election Manager**

    class ElectionManager {
    public:
        virtual void start_election() = 0;

        virtual void handle_vote_response(
            const RequestVoteResponse& response
        ) = 0;

        virtual void reset_election_timeout() = 0;

        virtual ~ElectionManager() = default;
    };

**State Machine**

    class StateMachine {
    public:
        virtual void apply(
            const RaftLogEntry& entry
        ) = 0;

        virtual uint64_t last_applied() const = 0;

        virtual ~StateMachine() = default;
    };

The implementation may use concrete classes instead of virtual interfaces if that better suits the module architecture.

### Implementation Phases

**Phase 1: Raft Foundations**

- Define node roles and transitions.
- Define term and log-index types.
- Define Raft log entry format.
- Implement log serialization.
- Implement local Raft log storage.
- Implement persistent term and vote metadata.
- Implement log recovery.

**Phase 2: Leader Election**

- Implement election timers.
- Implement randomized timeout handling.
- Implement RequestVote RPC.
- Implement vote eligibility.
- Implement persistent vote handling.
- Implement candidate transitions.
- Implement leader initialization.
- Test election safety.

**Phase 3: Heartbeats**

- Implement periodic heartbeat scheduling.
- Implement empty AppendEntries.
- Implement follower timeout reset.
- Implement term validation.
- Implement heartbeat failure handling.
- Test leader failure detection.

**Phase 4: Log Replication**

- Implement AppendEntries RPC.
- Implement previous-index and previous-term validation.
- Implement log matching.
- Implement follower log persistence.
- Implement `next_index` and `match_index`.
- Implement conflict resolution.
- Implement commit-index advancement.
- Implement ordered state-machine application.

**Phase 5: Failure Handling**

- Test leader crashes.
- Test follower crashes.
- Test network partitions.
- Test simultaneous elections.
- Test stale leaders.
- Test uncommitted suffix replacement.
- Test node restart.
- Test preservation of committed entries.

**Phase 6: Membership Changes**

- Implement cluster configuration metadata.
- Implement learner nodes.
- Implement joint consensus.
- Implement node addition.
- Implement node removal.
- Test configuration transitions.

**Phase 7: Snapshots and Compaction**

- Define snapshot format.
- Implement snapshot creation.
- Implement snapshot persistence.
- Implement InstallSnapshot RPC.
- Implement follower snapshot installation.
- Implement log compaction.
- Coordinate snapshots with database checkpoints.

**Phase 8: Database Integration**

- Integrate Raft log persistence with WAL.
- Integrate the state machine with B+Tree.
- Integrate the state machine with LSM.
- Coordinate database transactions with replicated operations.
- Implement request deduplication.
- Add replication metrics.
- Test crash consistency across consensus and storage.

### Testing Strategy

Raft requires deterministic testing and failure injection.

**Unit Tests**

- Term increments.
- Vote eligibility.
- Candidate log freshness.
- Election transitions.
- Higher-term handling.
- Heartbeat timeout reset.
- AppendEntries prefix matching.
- Conflict detection.
- Commit-index advancement.
- Persistent-state recovery.

**Integration Tests**

- Three-node leader election.
- Leader heartbeat delivery.
- Write replication.
- Follower catch-up.
- Leader restart.
- Follower restart.
- Network partition and healing.
- Majority loss.
- Membership changes.
- Snapshot installation.

**Fault-Injection Tests**

- Crash before term persistence.
- Crash after term persistence.
- Crash before vote persistence.
- Crash after vote persistence.
- Crash before WAL flush.
- Crash after local log persistence but before replication.
- Crash after follower persistence but before acknowledgment.
- Crash after majority replication but before commit-index advancement.
- Crash after commitment but before state-machine application.
- Crash during snapshot installation.
- Delayed and reordered RPC responses.
- Duplicate RPC delivery.

**Core Invariants**

1. At most one leader is elected in a term.
2. A node grants at most one vote per term.
3. A committed entry is never overwritten.
4. Log entries are applied in index order.
5. Two nodes never apply different entries at the same index.
6. Every new leader contains all previously committed entries.
7. A follower never acknowledges an entry before the required persistence operation.
8. A minority partition cannot commit new entries.
9. Higher-term information forces outdated leaders and candidates to step down.
10. Recovery restores a state consistent with the committed log.

---

## Open Questions

### Node Architecture

1. Should each Raft node use a single-threaded event loop?
2. Should elections, heartbeats, and replication use separate threads?
3. How should Raft state transitions be synchronized?
4. Should RPC handling be synchronous or asynchronous?
5. Should node identity be assigned during cluster initialization?
6. How should node identity persist across restarts?
7. Should the initial implementation support one consensus group only?
8. How should cluster configuration be loaded and validated?

### Leader Election

1. What should the initial election timeout range be?
2. What should the heartbeat interval be?
3. Should pre-vote be implemented?
4. How should simultaneous elections be handled?
5. Should a leader step down after prolonged quorum loss?
6. How should a stale leader be prevented from serving reads?
7. Should leadership transfer be supported?
8. How should election metrics be exposed?
9. How should election timeouts adapt to deployment conditions?
10. Should a node perform additional health checks before starting an election?

### Log Replication

1. Should AppendEntries requests be batched?
2. Should multiple replication requests be in flight per follower?
3. Should the leader use pipelined replication?
4. How should slow followers be handled?
5. Should replication apply backpressure to client writes?
6. Should log entries be compressed?
7. How should large operation payloads be replicated?
8. Should the system support zero-copy network transfers?
9. How should log checksums be computed?
10. How should log segment allocation work?

### WAL and Persistence

1. Should the Raft log be separate from the database WAL?
2. Should Raft entries contain logical operations or physical WAL records?
3. How should Raft persistence coordinate with database recovery?
4. Should term and vote metadata be stored in a separate metadata file?
5. Should log entries be group-committed?
6. How should torn writes be detected?
7. How should log truncation interact with WAL recovery?
8. How should the system recover from a partially written entry?
9. Should the log use fixed-size segments?
10. How should checkpoint metadata coordinate with Raft snapshots?

### Membership Changes

1. When should joint consensus be implemented?
2. Should learner nodes be supported?
3. How should nodes be added safely?
4. How should nodes be removed safely?
5. How should configuration changes be persisted?
6. Should membership changes be initiated by an administrator or through an API?
7. How should a node rejoin after an extended outage?
8. How should quorum changes be coordinated?
9. Should automatic replica promotion be supported?
10. How should cluster recovery work after permanent loss of a majority?

### Failure Handling

1. How should repeated leader failures be handled?
2. Should nodes detect disk failures independently of network failures?
3. How should a follower behave when its local log is corrupt?
4. Should a corrupt follower automatically rebuild from a snapshot?
5. How should the cluster respond to delayed and duplicated RPCs?
6. How should a minority partition rejoin?
7. How should permanent node loss be distinguished from temporary unavailability?
8. Should a node be allowed to rejoin with an empty local log?
9. How should operators recover a cluster when no majority is available?
10. What safeguards should exist against accidental cluster reinitialization?

### Consistency and Reads

1. Should linearizable reads use ReadIndex?
2. Should follower reads be supported initially?
3. Should stale reads expose the follower's applied index?
4. Should clients specify a minimum required read index?
5. Should leader leases be considered?
6. How should Read Your Writes be guaranteed after failover?
7. Should bounded-staleness reads be supported?
8. How should MVCC snapshots interact with replicated log indexes?
9. How should transaction visibility be coordinated across replicas?
10. Should read routing be handled by the client or Replication Manager?

### Snapshots and Recovery

1. Should snapshots contain complete database state or reference database checkpoints?
2. How should snapshot creation coordinate with the checkpoint manager?
3. Should snapshot transfer be chunked?
4. How should snapshot checksums be validated?
5. Should snapshots be compressed?
6. How should snapshot installation be made atomic?
7. When can old log segments be deleted?
8. How should recovery proceed if a snapshot is corrupt?
9. Should followers recover from other followers?
10. How should snapshot metadata preserve cluster membership information?

### Database Integration

1. Should Raft replicate logical database operations or database WAL records?
2. How should Raft commitment coordinate with transaction commit?
3. How should multi-operation transactions be applied atomically?
4. How should Raft integrate with MVCC?
5. How should B+Tree updates remain deterministic across replicas?
6. How should LSM sequence numbers be assigned consistently?
7. How should compaction interact with replication snapshots?
8. How should database checkpoints and Raft snapshots be coordinated?
9. How should client request deduplication survive recovery?
10. Should Raft integration begin before or after full transaction and recovery support?

### Future Extensions

1. Should MiniDB support multiple Raft groups?
2. Should replication support automatic sharding?
3. Should cross-shard transactions be supported?
4. Should geo-distributed clusters be supported?
5. Should follower replicas support analytical workloads?
6. Should asynchronous read replicas exist outside the consensus group?
7. Should disaster-recovery replication between independent clusters be supported?
8. How should Raft integrate with distributed query execution?
9. Should dynamic quorum and replica-placement policies be supported?
10. Should MiniDB eventually support multi-region database deployments?