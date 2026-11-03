# Replication

## Overview

Replication is the process of maintaining multiple copies of database data across independent nodes to provide high availability, fault tolerance, and resilience against node failures.

MiniDB will use a leader-based replication architecture built around the Raft consensus protocol.

A cluster consists of multiple database nodes. At any point in time, one node acts as the leader while the remaining nodes act as followers. The leader coordinates write operations, establishes their ordering, and replicates the resulting log entries to followers.

Followers persist replicated entries and acknowledge successful replication. Once an entry satisfies Raft's commitment conditions, it becomes committed and can be applied to the database state machine.

Replication separates the logical ordering of database operations from their physical execution on individual nodes.

The leader establishes a consistent sequence of committed operations. Each replica applies that sequence in the same order to maintain an equivalent logical database state.

### Core Concepts

| Concept | Description |
|---|---|
| Cluster | Collection of database nodes participating in replication |
| Node | Individual MiniDB database instance |
| Leader | Node responsible for coordinating writes and replication |
| Follower | Node that receives and persists replicated entries |
| Raft Log | Ordered sequence of operations agreed upon by the cluster |
| Log Index | Position of an entry in the replicated log |
| Term | Logical election epoch associated with a log entry |
| Commit Index | Highest log index known to be committed |
| Applied Index | Highest committed log index applied to the state machine |
| Quorum | Minimum number of nodes required for a consensus decision |
| WAL | Write-Ahead Log used for durable persistence and recovery |
| Replication Lag | Difference between leader and follower replication progress |
| Snapshot | Compacted representation of the database state |
| Failover | Process of electing a new leader after a failure |
| Replica Recovery | Process of bringing an outdated or restarted replica up to date |

### Architecture

    +-----------------------------------------------------------+
    |                     MiniDB Cluster                        |
    |                                                           |
    |     +----------------------+                              |
    |     |        Leader        |                              |
    |     |                      |                              |
    |     | Client Requests      |                              |
    |     | Replication Manager  |                              |
    |     | Raft Log             |                              |
    |     | WAL                  |                              |
    |     +----------------------+                              |
    |                |                                          |
    |          +-----+-----+                                    |
    |          |           |                                    |
    |          v           v                                    |
    |    +-----------+  +-----------+                           |
    |    | Follower  |  | Follower  |                           |
    |    |           |  |           |                           |
    |    | Raft Log  |  | Raft Log  |                           |
    |    | WAL       |  | WAL       |                           |
    |    +-----------+  +-----------+                           |
    |                                                           |
    +-----------------------------------------------------------+
                          |
                          v
    +-----------------------------------------------------------+
    |                 Database State Machine                    |
    +-----------------------------------------------------------+
                          |
                          v
    +-----------------------------------------------------------+
    |          B+Tree | LSM Tree | Buffer Pool                   |
    +-----------------------------------------------------------+

### Replication Layers

**Replication Manager**

Coordinates replication across cluster nodes.

Responsibilities:
- Maintain peer connections.
- Route client write requests to the leader.
- Schedule log replication.
- Track follower progress.
- Coordinate replication recovery.
- Expose replication health and metrics.

**Raft Consensus Engine**

Establishes the ordering and commitment of replicated operations.

Responsibilities:
- Leader election.
- Term management.
- Heartbeats.
- Log replication.
- Commit-index advancement.
- Membership coordination.

**Raft Log**

Maintains the ordered sequence of replicated operations.

Responsibilities:
- Log entry creation.
- Log indexing.
- Persistent log storage.
- Log retrieval.
- Conflict resolution.
- Log compaction.

**WAL Manager**

Provides durable persistence and crash recovery.

Responsibilities:
- Persist log records.
- Flush records to stable storage.
- Detect incomplete records.
- Validate checksums.
- Recover persisted state.

**State Machine**

Applies committed operations to the database.

Responsibilities:
- Apply entries in log order.
- Maintain applied-index metadata.
- Execute deterministic database operations.
- Coordinate with the storage engine.

**Storage Engine**

Maintains the physical database state.

Responsibilities:
- B+Tree and LSM operations.
- Buffer-pool management.
- Page persistence.
- Index maintenance.
- Database checkpointing.

---

## Goals

1. Maintain multiple copies of database data across independent nodes.
2. Provide high availability through leader-based replication.
3. Maintain a consistent ordering of committed operations.
4. Ensure committed operations survive individual node failures, subject to quorum and durability assumptions.
5. Support automatic leader election and failover.
6. Replicate WAL records or equivalent deterministic operations.
7. Support follower recovery after temporary disconnection.
8. Detect and repair inconsistent follower logs.
9. Support snapshot-based recovery for significantly outdated replicas.
10. Separate consensus decisions from physical storage implementation.
11. Integrate replication with WAL, checkpointing, and crash recovery.
12. Support consistent state-machine application across replicas.
13. Provide replication progress and health information.
14. Support configurable read consistency.
15. Establish a foundation for distributed database capabilities.

### Non-Goals for Initial Implementation

- Multi-leader replication.
- Active-active writes.
- Byzantine fault tolerance.
- Automatic sharding.
- Cross-shard transactions.
- Geo-distributed consensus optimization.
- Distributed query execution.
- Multi-cluster replication.
- Conflict-free replicated data types.

### Initial Cluster Configuration

The initial implementation targets a three-node cluster.

| Property | Initial Configuration |
|---|---|
| Cluster size | 3 nodes |
| Leader count | 1 |
| Followers | 2 |
| Quorum | 2 |
| Failure tolerance | 1 node |
| Consensus protocol | Raft |
| Replication topology | Leader-follower |
| Write coordination | Leader |
| Log replication | Raft AppendEntries |
| Persistence | Local durable WAL |
| Membership | Static initially |

For a cluster containing N voting members:

    quorum = floor(N / 2) + 1

| Cluster Size | Quorum | Tolerated Failures |
|---:|---:|---:|
| 1 | 1 | 0 |
| 3 | 2 | 1 |
| 5 | 3 | 2 |
| 7 | 4 | 3 |

A cluster can continue committing writes only while a majority of voting members can communicate and satisfy the required persistence conditions.

---

## Leader-Follower Architecture

MiniDB uses a leader-based replication architecture in which one node coordinates writes and the remaining nodes replicate its committed history.

The leader is elected through Raft consensus.

Followers receive log entries, persist them, acknowledge successful replication, and apply committed operations to their local state machines.

### Leader Responsibilities

- Accept client write requests.
- Assign log indexes and terms.
- Append operations to the local Raft log.
- Persist log entries.
- Replicate entries to followers.
- Track follower replication progress.
- Determine when entries become committed.
- Advance the commit index.
- Apply committed operations.
- Send periodic heartbeats.
- Detect follower failures.
- Coordinate follower recovery.
- Respond to clients according to configured durability and consistency guarantees.

### Follower Responsibilities

- Receive AppendEntries RPCs.
- Validate incoming log entries.
- Persist accepted entries.
- Acknowledge successful replication.
- Apply committed entries in order.
- Receive commit-index updates.
- Monitor leader heartbeats.
- Participate in leader elections.
- Recover missing log entries.
- Install snapshots when required.

Followers do not independently commit client writes.

### Leader Replication State

The leader maintains replication progress for every follower.

Conceptual representation:

    struct FollowerProgress {
        NodeId node_id;

        uint64_t next_index;
        uint64_t match_index;
    };

    struct LeaderReplicationState {
        std::unordered_map<NodeId, FollowerProgress> followers;
    };

`next_index` represents the next log index the leader intends to send to a follower.

`match_index` represents the highest log index known to be replicated on that follower.

### Leader Initialization

When a node becomes leader:

1. Initialize `next_index` for every follower to the leader's last log index plus one.
2. Initialize `match_index` to the known matching prefix.
3. Start sending heartbeats.
4. Append a no-op entry in the new term.
5. Begin replicating pending entries.
6. Advance the commit index when Raft's commitment conditions are satisfied.

### Write Routing

All writes must pass through the current leader.

    Client
       |
       v
    Receiving Node
       |
       v
    Is Current Leader?
       |
       +---- Yes ----> Process Write
       |
       +---- No -----> Redirect / Forward
                          |
                          v
                     Current Leader

If a follower receives a write request, it may:

- Return the current leader's address.
- Forward the request to the leader.

The initial implementation should use a clearly defined leader-redirection or forwarding mechanism.

### Read Routing

Reads may be served by the leader or followers depending on the requested consistency level.

| Read Mode | Behavior |
|---|---|
| Leader Read | Read from the leader's local database |
| Linearizable Read | Validate current leader authority and applied state |
| Follower Read | Read from a follower's local state |
| Stale Read | Allow reads behind the latest committed state |
| Read Your Writes | Ensure the client observes its acknowledged writes |

A leader-local read is not automatically linearizable. The leader must establish that it still has valid authority.

Follower reads may be stale because a follower can lag behind the leader's commit or applied index.

### Architectural Separation

Replication must remain independent of the physical storage implementation.

    +------------------------------------------------+
    |               Client Interface                 |
    +------------------------------------------------+
                         |
                         v
    +------------------------------------------------+
    |              Replication Manager               |
    +------------------------------------------------+
                         |
                         v
    +------------------------------------------------+
    |               Raft Consensus Engine            |
    +------------------------------------------------+
                         |
                         v
    +------------------------------------------------+
    |                    Raft Log                    |
    +------------------------------------------------+
                         |
                         v
    +------------------------------------------------+
    |                  WAL / Recovery                |
    +------------------------------------------------+
                         |
                         v
    +------------------------------------------------+
    |                 State Machine                  |
    +------------------------------------------------+
                         |
                         v
    +------------------------------------------------+
    |               Database Storage                |
    |                                                |
    |          B+Tree | LSM | Buffer Pool             |
    +------------------------------------------------+

The Raft layer determines the committed operation sequence.

The state machine applies committed operations.

The storage engine determines how those operations are physically represented.

---

## WAL Shipping

WAL shipping is the mechanism through which the leader transfers its ordered log entries to followers.

The leader appends each operation to its local log, persists it, and sends it to followers using AppendEntries RPCs.

Followers validate the log prefix, append accepted entries, persist them, and acknowledge successful replication.

### Replication Model

The initial implementation uses a Raft-managed replicated log.

    Client Write
        |
        v
    Leader
        |
        v
    Append Raft Log Entry
        |
        v
    Persist Local WAL
        |
        v
    Send AppendEntries RPC
        |
        +-------------------------+
        |                         |
        v                         v
      Follower B                Follower C
        |                         |
        v                         v
    Persist WAL               Persist WAL
        |                         |
        v                         v
    Acknowledge               Acknowledge
        |                         |
        +------------+------------+
                     |
                     v
                   Leader
                     |
                     v
              Majority Confirmed
                     |
                     v
                Commit Entry
                     |
                     v
              Apply to State Machine

### Raft Log Entry

Each replicated entry contains metadata and an operation payload.

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

The payload must use an explicitly defined serialization format.

The persistent format must not depend on compiler-specific C++ struct layout or raw in-memory object representations.

### WAL Record Format

A persistent record may contain:

    +------------------------------------------------+
    |                  WAL Record                    |
    +------------------------------------------------+
    | Record Length                                  |
    +------------------------------------------------+
    | Log Index                                      |
    +------------------------------------------------+
    | Term                                           |
    +------------------------------------------------+
    | Entry Type                                     |
    +------------------------------------------------+
    | Payload Length                                 |
    +------------------------------------------------+
    | Payload                                        |
    +------------------------------------------------+
    | Checksum                                       |
    +------------------------------------------------+

The format must define:

- Byte order.
- Record version.
- Maximum record size.
- Payload serialization.
- Checksum algorithm.
- Torn-write detection.
- Recovery behavior for incomplete records.

### Replication Acknowledgment

A follower acknowledges an entry only after the entry has been durably persisted according to the configured storage durability contract.

An acknowledgment must not merely indicate that an entry was received in memory.

The leader uses acknowledgments to update the follower's replication progress.

### Commit Index

The commit index represents the highest log index known to be committed.

The leader may advance its commit index when:

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

The Raft log and database state machine have separate responsibilities.

The Raft log determines which operations are committed.

The state machine applies committed operations to the database.

    Raft Log
       |
       v
    Commit Index
       |
       v
    Apply Entries in Log Order
       |
       v
    Database State Machine
       |
       v
    Database Storage

Every node applies committed entries in log-index order.

The `last_applied` index tracks the highest entry applied by the local state machine.

The following invariant must always hold:

    last_applied <= commit_index

### Raft Log and Database WAL

MiniDB already has a page-oriented WAL design for storage recovery.

Replication introduces the additional requirement of maintaining a common, committed ordering of operations across nodes.

Two logical records must be distinguished:

**Raft Log**

- Establishes consensus ordering.
- Stores terms and log indexes.
- Supports conflict detection and reconciliation.
- Determines which operations are committed.

**Database WAL**

- Supports physical or logical database recovery.
- Records page or logical database modifications.
- Supports crash recovery.
- Coordinates with database checkpoints.

Two physical implementation approaches are possible.

#### Approach A: Replicate the Database WAL Directly

The database WAL records become the replicated log.

Advantages:

- Less duplicated logging.
- Direct relationship between replication and database recovery.
- Potentially lower storage overhead.

Challenges:

- Existing WAL semantics must satisfy Raft's log requirements.
- Physical WAL records may not map cleanly to deterministic state-machine commands.
- Raft conflict repair becomes coupled to database recovery.
- Log truncation and checkpointing become more complex.

#### Approach B: Maintain a Separate Raft Log

The Raft log stores ordered logical operations or references to database WAL operations.

Advantages:

- Clear separation between consensus and storage recovery.
- Easier reasoning about terms and log indexes.
- Storage engines remain independent of consensus implementation.
- Easier evolution of database WAL formats.

Challenges:

- Additional persistence and storage overhead.
- Requires coordination between Raft commitment and database WAL durability.
- Requires a well-defined recovery protocol between both logs.

The initial architecture should preserve separate logical interfaces even if the implementation later shares physical WAL infrastructure.

### Write Acknowledgment

The leader must not acknowledge a successful durable write before the operation satisfies the configured commitment and persistence requirements.

Baseline workflow:

1. Append the operation to the Raft log.
2. Persist the local entry.
3. Replicate it to followers.
4. Receive durable acknowledgments from a majority.
5. Advance the commit index.
6. Apply the operation to the local state machine.
7. Return success to the client.

The client-facing contract must specify whether the response waits for state-machine application or only for durable commitment.

For the initial implementation, success should mean that the operation is committed and durable according to the configured cluster durability contract.

---

## Replica Recovery

Replica recovery is the process of bringing a restarted, disconnected, or outdated follower back into synchronization with the current leader.

A follower may become outdated because of:

- Process crashes.
- Machine restarts.
- Network partitions.
- Temporary connectivity failures.
- Disk failures.
- WAL corruption.
- Extended replication lag.

When the follower reconnects, it must reconcile its local log with the leader's log.

### Recovery Goals

1. Restore missing log entries.
2. Remove conflicting uncommitted entries.
3. Preserve committed entries.
4. Restore a consistent state-machine position.
5. Resume normal replication.
6. Avoid incorrectly reapplying operations.
7. Support snapshot-based recovery when necessary.

### Log Reconciliation

The leader and follower compare their log histories using the previous log index and term.

Recovery workflow:

1. The leader sends AppendEntries with the expected previous log index and term.
2. The follower verifies the previous log entry.
3. If the prefix matches, the follower accepts new entries.
4. If the prefix does not match, the follower rejects the request.
5. The leader moves `next_index` backward.
6. The leader retries until a matching prefix is found.
7. The follower removes conflicting uncommitted entries.
8. The follower appends the leader's entries.
9. The follower persists the new entries.
10. Normal replication resumes.

### Example

    Leader:

        Index:  1   2   3   4   5   6
        Term:   1   1   2   2   3   3

    Follower:

        Index:  1   2   3   4
        Term:   1   1   2   1

    Matching prefix: index 1 through 3.

    Conflicting entry: index 4.

    Leader sends the correct suffix.

    Follower replaces the conflicting uncommitted suffix
    and appends the leader's entries.

A follower must never remove or overwrite a committed entry.

### Recovery After Process Restart

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
10. Contact the current leader.
11. Reconcile its log.
12. Resume replication.

A restarted node must not assume that every locally persisted entry is committed.

### Snapshot-Based Recovery

A follower may be too far behind for the leader to retain all required log entries.

In this case, the leader transfers a snapshot.

A snapshot represents the database state after applying all log entries up to a specific index.

Conceptual metadata:

    struct SnapshotMetadata {
        uint64_t last_included_index;
        uint64_t last_included_term;

        uint64_t configuration_index;
    };

The snapshot must preserve the log position and cluster configuration represented by the snapshot.

### Snapshot Installation

    Follower
       |
       v
    Detect Missing Log Prefix
       |
       v
    Request Snapshot
       |
       v
    Receive Snapshot
       |
       v
    Validate Snapshot
       |
       v
    Persist Snapshot
       |
       v
    Update Applied Index
       |
       v
    Reconcile Remaining Log Entries
       |
       v
    Resume Replication

Snapshot installation must be crash-safe.

A partially received snapshot must never replace a valid existing database state.

### Snapshot and WAL Coordination

Snapshots and log compaction must be coordinated.

After a snapshot is safely persisted:

- Entries before the snapshot's included index may be compacted when Raft safety conditions permit.
- The snapshot retains the last included index and term.
- Remaining log entries continue from the snapshot boundary.
- Recovery reconstructs the state machine from the snapshot and remaining log.

### Database State Recovery

Raft recovery and database recovery must agree on the state-machine position.

The database must not expose an operation as committed if it was not committed by Raft.

Likewise, a committed operation must not be permanently lost from the state machine.

The implementation should persist sufficient metadata to coordinate:

- Raft commit index.
- Raft applied index.
- Database checkpoint position.
- Database WAL recovery position.
- Snapshot included index.

---

## Failover

Failover is the process through which the cluster elects a new leader when the existing leader becomes unavailable.

Raft uses terms, election timeouts, and majority voting to select a new leader.

### Leader Failure

Consider a three-node cluster.

    Before Failure:

        Node A: Leader
        Node B: Follower
        Node C: Follower

    Node A becomes unavailable.

    Followers detect election timeout.

    After Election:

        Node A: Unavailable
        Node B: Leader
        Node C: Follower

The cluster can continue committing writes because B and C form a majority.

### Failure Detection

Followers monitor the leader through periodic AppendEntries heartbeats.

If a follower does not receive valid leader communication within its election timeout, it may initiate an election.

Election timeouts should be randomized to reduce simultaneous elections.

Illustrative configuration:

    Heartbeat Interval:       100 ms
    Minimum Election Timeout: 300 ms
    Maximum Election Timeout: 600 ms

These values are initial testing parameters, not production recommendations.

### Failover Workflow

    +---------------------+
    | Current Leader      |
    | Node A              |
    +---------------------+
              |
              | Leader unavailable
              v
    +---------------------+
    | Followers Detect    |
    | Election Timeout    |
    +---------------------+
              |
              v
    +---------------------+
    | Candidate Starts    |
    | New Term            |
    +---------------------+
              |
              v
    +---------------------+
    | Request Votes       |
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
    | New Leader|
    +-----------+
          |
          v
    Resume Replication

### Network Partition

Consider a three-node cluster split into two partitions.

    Partition A:             Partition B:

    Node A (Leader)          Node B (Follower)
                             Node C (Follower)

The majority partition contains B and C.

The minority partition contains A.

The majority partition can elect a new leader and continue committing writes.

The isolated old leader cannot commit new entries because it cannot obtain a majority.

When communication is restored:

1. The old leader receives a higher term.
2. It transitions to follower state.
3. Its uncommitted conflicting entries are reconciled.
4. The new leader's committed history is preserved.

### Failover Guarantees

- Only a candidate receiving a majority can become leader.
- A new leader contains all previously committed entries.
- A minority partition cannot commit new writes.
- An old leader steps down after observing a higher term.
- Uncommitted entries may be discarded during log reconciliation.
- Committed entries must never be discarded.

### Client Behavior During Failover

Clients may experience:

- Connection failures.
- Leader redirection.
- Temporary write unavailability.
- Request timeouts.
- Ambiguous responses when a connection fails after commitment.

The client interface should support:

- Leader discovery.
- Retryable errors.
- Request timeouts.
- Idempotency keys or request identifiers.
- Duplicate-request handling.

A client retry must not accidentally execute a logical write twice.

The request identity and deduplication state must be replicated and persisted for the required retention period.

### Failover and Durability

A committed entry must survive leader failure if the remaining cluster retains a quorum and the configured persistence assumptions hold.

This depends on:

- Durable local log persistence.
- Correct quorum acknowledgment.
- Correct commit-index advancement.
- Correct leader election.
- Correct log reconciliation.
- Correct state-machine recovery.

Raft does not protect against simultaneous permanent loss of a majority of replicas holding the committed data.

---

## Consistency Guarantees

Replication must maintain a common ordering of committed operations across the cluster.

Raft provides the consensus mechanism that establishes this ordering.

MiniDB's state machines apply committed entries in the same order to maintain equivalent logical database states.

### Strongly Ordered Writes

All committed operations are ordered by their position in the Raft log.

If operation A is committed before operation B, every replica applies A before B.

    Leader Log:

        Index 1: INSERT key=100
        Index 2: UPDATE key=100
        Index 3: DELETE key=100

    All replicas apply:

        INSERT
          |
          v
        UPDATE
          |
          v
        DELETE

### Committed Write Durability

A write can be acknowledged as committed after it has been replicated to a majority and the required durability conditions have been satisfied.

A committed entry must be preserved across leader changes.

### Linearizable Writes

A successful write acknowledgment can represent a linearizable write when:

- The operation has been committed by the Raft group.
- The leader's authority is valid.
- The write is acknowledged according to the required commitment and durability rules.
- The client-visible operation has a well-defined linearization point.

Raft log replication alone does not automatically make every database operation linearizable.

### Read Consistency

The replication layer must distinguish between read consistency modes.

| Read Mode | Guarantee |
|---|---|
| Leader Read | Served by the leader; requires authority validation for linearizability |
| Linearizable Read | Validated against current cluster authority |
| Follower Read | May return a state behind the latest commit |
| Stale Read | Explicitly permits outdated results |
| Read Your Writes | Client observes its previously acknowledged writes |

For linearizable reads, the initial design should use a quorum-based ReadIndex mechanism or an appropriately implemented leader lease with explicit timing assumptions.

A follower must not serve a read as linearizable merely because it has recently received a heartbeat.

### Commit Versus Apply

The implementation must distinguish between committed and applied entries.

**Committed**

The operation is agreed upon by the required quorum and belongs to the committed log.

**Applied**

The operation has been executed by the local database state machine.

A node may have:

    commit_index > last_applied

This means committed operations are waiting to be applied locally.

The following invariant must hold:

    last_applied <= commit_index

The state machine must apply entries in order.

### Replication Lag

Replication lag measures the difference between the leader's progress and a follower's progress.

Useful metrics include:

- Leader last log index.
- Follower last log index.
- Leader commit index.
- Follower commit index.
- Leader applied index.
- Follower applied index.
- Replication acknowledgment latency.
- Time since last successful heartbeat.

Replication lag affects:

- Follower read freshness.
- Replica recovery time.
- Failover readiness.
- Snapshot requirements.

### Deterministic State Machine

All replicas must produce equivalent logical states after applying the same committed entries.

Replicated operations must be deterministic.

Potential sources of nondeterminism include:

- Local wall-clock timestamps.
- Random number generation.
- Unordered data-structure iteration.
- Node-specific file paths.
- Independent sequence-number allocation.
- Environment-dependent operation behavior.

Values that must remain consistent across replicas should be established by the leader and included in the replicated payload, or derived deterministically.

### Consistency Guarantees by Operation

| Operation | Expected Guarantee |
|---|---|
| Committed write | Present in the committed Raft log |
| Committed ordering | Identical across replicas |
| Leader election | New leader contains committed entries |
| Follower replication | Eventually catches up when communication and resources permit |
| Follower read | May be stale |
| Linearizable read | Requires current authority validation |
| Uncommitted write | May be discarded during failover |
| Minority partition write | Cannot be committed |

---

## Replication Workflow

This section defines the end-to-end lifecycle of write requests, log replication, commitment, state-machine application, reads, and recovery.

### 1. Client Write

The client submits a write request to the cluster.

    Client
       |
       v
    Receiving Node
       |
       v
    Identify Current Leader
       |
       v
    Forward or Redirect Request
       |
       v
    Leader Processes Request

If the receiving node is not the leader, it forwards the request or returns the leader's address according to the client protocol.

### 2. Append to Raft Log

The leader converts the request into a deterministic operation.

The leader:

1. Validates the request.
2. Assigns the next log index.
3. Attaches the current term.
4. Serializes the operation.
5. Appends the entry to the local Raft log.
6. Persists the entry.

### 3. Replicate to Followers

The leader sends AppendEntries RPCs to followers.

Each follower:

1. Checks the term.
2. Verifies the previous log index and term.
3. Rejects requests with inconsistent prefixes.
4. Removes conflicting uncommitted suffixes when required.
5. Appends new entries.
6. Persists the entries.
7. Acknowledges successful replication.

### 4. Commit the Entry

The leader tracks successful follower acknowledgments.

When a majority has replicated the entry and the Raft commitment rules are satisfied, the leader advances its commit index.

### 5. Apply the Entry

The leader applies committed entries to its local state machine in log order.

Followers receive updated commit information and apply committed entries locally.

### 6. Respond to Client

The leader returns success after the operation has satisfied the configured commitment and durability requirements.

The response should contain enough information to support request deduplication and client-side retry behavior.

### Complete Write Workflow

    +----------------------+
    |     Client Write     |
    +----------------------+
               |
               v
    +----------------------+
    |   Identify Leader    |
    +----------------------+
               |
               v
    +----------------------+
    | Serialize Operation |
    +----------------------+
               |
               v
    +----------------------+
    | Append to Raft Log   |
    +----------------------+
               |
               v
    +----------------------+
    | Persist Local WAL    |
    +----------------------+
               |
               v
    +----------------------+
    | Replicate to Peers   |
    +----------------------+
               |
               v
    +----------------------+
    | Persist on Followers |
    +----------------------+
               |
               v
    +----------------------+
    | Majority Acknowledged|
    +----------------------+
               |
               v
    +----------------------+
    | Advance Commit Index |
    +----------------------+
               |
               v
    +----------------------+
    | Apply to State       |
    | Machine              |
    +----------------------+
               |
               v
    +----------------------+
    | Return Success       |
    +----------------------+

### Read Workflow

The read path depends on the requested consistency level.

**Linearizable Read**

    Client
       |
       v
    Current Leader
       |
       v
    Establish Current Authority
       |
       v
    Ensure Applied Index >= Required Commit Index
       |
       v
    Execute Database Read
       |
       v
    Return Result

**Follower Read**

    Client
       |
       v
    Follower
       |
       v
    Check Local Applied Index
       |
       v
    Execute Read Against Local State
       |
       v
    Return Result with Read Position

Follower reads may be stale unless the client supplies a minimum required index or the follower performs additional synchronization.

### Heartbeat Workflow

The leader periodically sends AppendEntries requests, including requests without new log entries.

Followers use these messages to:

- Confirm leader availability.
- Learn the current term.
- Receive updated commit indexes.
- Maintain replication coordination.

If a follower does not receive valid leader communication within its election timeout, it may initiate an election.

### Transactional Write Integration

When transaction processing and MVCC are introduced, replication must account for transaction boundaries.

A logical transaction may contain multiple database operations.

The system must define whether the Raft log replicates:

- Individual physical WAL records.
- Individual logical operations.
- Complete transaction batches.
- Transaction commit records and associated operations.

For atomic transaction semantics, the state machine must not expose a partially applied transaction as committed.

A future implementation should represent transaction boundaries explicitly and ensure all replicas apply the same committed transaction outcome.

### Idempotency and Retry

A client may time out after a write is committed but before receiving its response.

The client may retry the operation.

To prevent duplicate execution, MiniDB should support a request identity.

    struct ClientRequestId {
        uint64_t client_id;
        uint64_t sequence_number;
    };

The replicated state machine can track completed request identities and return the previous result when it receives a duplicate request.

Deduplication state must survive leader changes and recovery for the required retention period.

---

## Design Decisions

| Area | Initial Decision |
|---|---|
| Replication architecture | Leader-follower |
| Consensus protocol | Raft |
| Cluster size | 3 nodes |
| Voting model | Majority quorum |
| Leader election | Randomized election timeout |
| Log replication | AppendEntries RPC |
| Election RPC | RequestVote |
| Heartbeats | Empty AppendEntries |
| Log ordering | Index and term |
| Commit rule | Majority replication with current-term restriction |
| State machine | Deterministic database operations |
| Write routing | Leader |
| Follower behavior | Replication and optional stale reads |
| Persistent Raft state | Current term, voted-for, log |
| Follower recovery | Log reconciliation |
| Large recovery | Snapshot installation |
| Log compaction | Snapshot-based |
| Membership changes | Deferred |
| Multi-leader replication | Not supported |
| Byzantine fault tolerance | Not supported |
| Linearizable reads | ReadIndex-style mechanism planned |

### Architectural Boundaries

**Replication Manager**

Responsible for:

- Peer management.
- Replication scheduling.
- Write routing.
- Follower progress.
- Replication health.
- Snapshot coordination.
- Replication metrics.

**Raft Consensus Engine**

Responsible for:

- Leader election.
- Term management.
- Vote processing.
- AppendEntries processing.
- Commit-index advancement.
- Membership configuration.

**Raft Log Manager**

Responsible for:

- Log entry creation.
- Log indexing.
- Log persistence.
- Log recovery.
- Conflict truncation.
- Log compaction.

**WAL Manager**

Responsible for:

- Durable record persistence.
- Record checksums.
- Recovery scanning.
- Torn-write detection.
- Flush and synchronization.

**State Machine**

Responsible for:

- Applying committed operations.
- Maintaining applied-index metadata.
- Executing deterministic operations.
- Coordinating with the storage engine.

**Database Storage Engine**

Responsible for:

- B+Tree and LSM operations.
- Buffer-pool management.
- Page persistence.
- Database-level recovery.
- Physical storage management.

### Suggested Module Structure

    minidb/
    |
    +-- replication/
    |   |
    |   +-- replication_manager.h
    |   +-- replication_manager.cpp
    |   |
    |   +-- raft_node.h
    |   +-- raft_node.cpp
    |   |
    |   +-- raft_log.h
    |   +-- raft_log.cpp
    |   |
    |   +-- raft_types.h
    |   |
    |   +-- leader_election.h
    |   +-- leader_election.cpp
    |   |
    |   +-- log_replication.h
    |   +-- log_replication.cpp
    |   |
    |   +-- snapshot_manager.h
    |   +-- snapshot_manager.cpp
    |   |
    |   +-- state_machine.h
    |   +-- state_machine.cpp
    |   |
    |   +-- peer_client.h
    |   +-- peer_client.cpp
    |   |
    |   +-- replication_metrics.h
    |   +-- replication_metrics.cpp
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

These interfaces describe component responsibilities and can evolve during implementation.

**Replication Manager**

    class ReplicationManager {
    public:
        virtual void submit_write(
            const std::vector<std::byte>& operation
        ) = 0;

        virtual void replicate() = 0;

        virtual void update_commit_index() = 0;

        virtual void apply_committed_entries() = 0;

        virtual ~ReplicationManager() = default;
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

**State Machine**

    class StateMachine {
    public:
        virtual void apply(
            const RaftLogEntry& entry
        ) = 0;

        virtual uint64_t last_applied() const = 0;

        virtual ~StateMachine() = default;
    };

The implementation may use concrete classes rather than virtual interfaces if that better fits the module architecture.

### Implementation Phases

**Phase 1: Replication Foundations**

- Define cluster and node identity.
- Define replication configuration.
- Define replication interfaces.
- Define Raft log entry serialization.
- Implement local log persistence.
- Implement persistent Raft metadata.
- Implement log recovery.

**Phase 2: Leader-Follower Communication**

- Implement peer connections.
- Implement RequestVote RPC.
- Implement AppendEntries RPC.
- Implement leader election.
- Implement heartbeats.
- Implement leader discovery.
- Test three-node communication.

**Phase 3: WAL Shipping and Log Replication**

- Implement log append.
- Implement follower log validation.
- Implement follower persistence.
- Implement replication acknowledgments.
- Implement follower progress tracking.
- Implement conflict resolution.
- Implement majority commitment.
- Implement ordered state-machine application.

**Phase 4: Failure Handling**

- Test leader crashes.
- Test follower crashes.
- Test network partitions.
- Test election retries.
- Test stale leaders.
- Test uncommitted entry truncation.
- Test restart recovery.
- Test committed-entry preservation.

**Phase 5: Replica Recovery**

- Implement follower catch-up.
- Implement snapshot format.
- Implement snapshot creation.
- Implement snapshot persistence.
- Implement InstallSnapshot RPC.
- Implement snapshot installation.
- Implement log compaction.
- Coordinate snapshots with database checkpoints.

**Phase 6: Database Integration**

- Integrate Raft log persistence with WAL.
- Integrate the state machine with B+Tree.
- Integrate the state machine with LSM.
- Coordinate transaction boundaries with replication.
- Implement client request deduplication.
- Add replication metrics and observability.
- Test crash consistency across replication and storage.

**Phase 7: Advanced Replication**

- Implement learner nodes.
- Implement joint consensus.
- Support dynamic membership.
- Implement linearizable reads.
- Support replication throttling.
- Improve batching and pipelining.
- Evaluate multi-Raft as a future extension.

### Testing Strategy

Replication correctness requires unit testing, integration testing, and fault injection.

**Unit Tests**

- Log entry serialization.
- Log indexing.
- Term validation.
- AppendEntries prefix matching.
- Conflict detection.
- Commit-index advancement.
- Follower progress tracking.
- Snapshot metadata.
- WAL recovery.

**Integration Tests**

- Three-node leader election.
- Write replication.
- Follower catch-up.
- Leader restart.
- Follower restart.
- Network partition and healing.
- Majority loss.
- Snapshot transfer.
- Database state-machine consistency.

**Fault-Injection Tests**

- Crash before local WAL flush.
- Crash after local persistence but before replication.
- Crash after follower persistence but before acknowledgment.
- Crash after majority replication but before commit-index advancement.
- Crash after commitment but before state-machine application.
- Crash during state-machine application.
- Partial snapshot transfer.
- WAL corruption.
- Delayed and reordered RPC responses.

**Core Invariants**

1. At most one leader exists in a term.
2. A committed log entry is never overwritten.
3. All committed entries are applied in order.
4. No two replicas apply different entries at the same log index.
5. A new leader contains all previously committed entries.
6. A follower never acknowledges an entry before the required persistence operation.
7. A minority partition cannot commit new writes.
8. Recovered state is consistent with the committed log.
9. Snapshot installation does not discard committed history.
10. Duplicate client requests do not execute a write more than once when deduplication is enabled.

---

## Open Questions

### Replication Architecture

1. Should the Replication Manager be independent of the Raft Node implementation?
2. Should replication use a single-threaded event loop or dedicated threads?
3. Should the initial deployment support only one Raft group?
4. How should node identity and peer endpoints be configured?
5. Should cluster membership be static initially?
6. Should clients communicate directly with the leader?
7. Should followers forward writes or return leader information?
8. How should the replication layer expose its state to the database?

### WAL and Persistence

1. Should the Raft log be separate from the database WAL?
2. Should Raft entries contain logical operations or physical WAL records?
3. How should Raft persistence coordinate with database recovery?
4. Should log entries be group-committed?
5. How should torn writes and incomplete records be detected?
6. Should log entries be stored in fixed-size segments?
7. How should log truncation interact with WAL recovery?
8. How should the system recover if Raft log and database WAL positions disagree?
9. Should checksums be calculated per entry or per segment?
10. How should database checkpoints coordinate with Raft snapshots?

### Replication Performance

1. Should follower replication be synchronous or pipelined?
2. Should multiple AppendEntries requests be in flight per follower?
3. Should log entries be batched?
4. Should compression be supported?
5. How should backpressure be applied to slow followers?
6. Should the leader throttle writes when replication lag becomes excessive?
7. How should replication bandwidth be measured?
8. Should replication and client traffic use separate network connections?
9. How should large operation payloads be replicated?
10. Should zero-copy network transfers be supported?

### Replica Recovery

1. Should snapshots contain complete database state or reference database checkpoints?
2. How should snapshot creation coordinate with the checkpoint manager?
3. Should snapshot transfer be chunked?
4. How should snapshot checksums be validated?
5. Should snapshots be compressed?
6. How should snapshot installation be made atomic?
7. When can old Raft log segments be deleted?
8. How should recovery proceed if a snapshot is corrupt?
9. Should a follower be able to recover from another follower?
10. How should snapshot metadata preserve cluster membership information?

### Failover

1. What should the initial election timeout range be?
2. What should the heartbeat interval be?
3. Should pre-vote be implemented?
4. Should a leader step down after prolonged quorum loss?
5. How should stale leaders be prevented from serving linearizable reads?
6. Should leadership transfer be supported?
7. How should clients discover a new leader?
8. How should ambiguous client responses after failover be handled?
9. Should request deduplication be implemented before exposing automatic retries?
10. How should failover metrics and history be exposed?

### Consistency and Reads

1. Should linearizable reads use ReadIndex?
2. Should follower reads be supported initially?
3. Should stale reads expose the follower's applied index?
4. Should clients specify a minimum required read index?
5. Should leader leases be considered?
6. How should Read Your Writes be guaranteed after failover?
7. Should bounded-staleness reads be supported?
8. How should replication interact with MVCC snapshots?
9. How should transaction visibility be coordinated across replicas?
10. Should read routing be handled by clients or the Replication Manager?

### Membership and Cluster Management

1. Should cluster membership remain static in the first implementation?
2. When should joint consensus be introduced?
3. Should learner nodes be supported?
4. How should nodes be added and removed?
5. How should node identity and endpoint changes be handled?
6. How should cluster configuration be persisted?
7. How should a node rejoin after an extended outage?
8. Should replica promotion be supported?
9. How should quorum changes be coordinated?
10. How should operators recover a cluster after permanent loss of a majority?

### Database Integration

1. Should Raft replicate database WAL records or deterministic logical operations?
2. How should Raft commitment coordinate with transaction commit?
3. How should multi-operation transactions be applied atomically?
4. How should Raft integrate with MVCC?
5. How should B+Tree updates remain deterministic across replicas?
6. How should LSM sequence numbers be assigned consistently?
7. How should compaction interact with replication snapshots?
8. How should database checkpoints and Raft snapshots be coordinated?
9. How should client request deduplication survive recovery?
10. Should replication be implemented before or after full transaction and recovery support?

### Observability and Operations

1. Which replication metrics should be exposed?
2. Should the system expose leader election history?
3. How should replication lag be reported?
4. Should alerts be generated for quorum loss?
5. How should slow or unhealthy followers be identified?
6. Should operators be able to trigger snapshots manually?
7. How should cluster health be represented?
8. Should replication diagnostics include per-follower log indexes?
9. How should the system expose commit and applied indexes?
10. What operational tools are required for cluster recovery?

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