# Distributed Storage

## Overview

Distributed Storage extends MiniDB from a single-node database engine into a distributed database system capable of storing, partitioning, replicating, and accessing data across multiple nodes.

The distributed storage layer is responsible for determining where data resides, how it is distributed across the cluster, how replicas are maintained, and how requests are routed to the appropriate nodes.

It builds on the existing storage engine and replication architecture while introducing cluster-level metadata and coordination.

The distributed architecture consists of the following major components:

- **Cluster Manager:** Maintains cluster membership, node identity, and cluster-level configuration.
- **Partition Manager:** Defines logical data partitions and their ownership.
- **Sharding Manager:** Maps logical shards to physical storage units.
- **Placement Manager:** Determines which nodes host primary and replica copies.
- **Tablet Manager:** Manages the lifecycle and movement of tablets.
- **Query Router:** Routes client requests to the nodes responsible for the requested data.
- **Transaction Coordinator:** Coordinates transactions spanning multiple partitions.
- **Failure Recovery Manager:** Handles node failures, replica recovery, and partition reassignment.
- **Replication Layer:** Maintains consistency between replicas, using the replication protocol defined in `replication.md` and `raft.md`.

### Architecture

    Client
      |
      v
    Query Router
      |
      v
    Cluster Metadata
      |
      v
    Partition / Shard Mapping
      |
      v
    Tablet Placement
      |
      +-----------------------------+
      |              |              |
      v              v              v
    Node A         Node B         Node C
      |              |              |
      v              v              v
    Tablet 1       Tablet 2       Tablet 3
      |              |              |
      v              v              v
    Local Storage  Local Storage  Local Storage
      |
      v
    WAL + Buffer Pool + Storage Engine

### Logical and Physical Organization

MiniDB distinguishes between the logical distribution of data and its physical storage.

The hierarchy is:

    Database
       |
       v
    Table / Collection
       |
       v
    Partition
       |
       v
    Shard
       |
       v
    Tablet
       |
       v
    Replica
       |
       v
    Storage Node

These concepts serve different purposes:

- **Partition:** A logical subset of a table's data, typically defined by a partitioning key or range.
- **Shard:** A unit of data distribution and ownership across the cluster.
- **Tablet:** A manageable physical or logical storage unit that can be assigned, moved, split, or replicated.
- **Replica:** A copy of a tablet maintained on another node.
- **Storage Node:** A machine or process responsible for hosting tablets.

The exact mapping between shards and tablets may evolve as the system supports dynamic splitting, merging, and rebalancing.

## Goals

### Horizontal Scalability

Distribute data across multiple nodes so that storage capacity and request throughput can grow with cluster size.

### Data Distribution

Support partitioning and sharding strategies that distribute data across nodes based on workload characteristics.

### Fault Tolerance

Maintain multiple copies of data so that the cluster can continue operating when individual nodes become unavailable.

### Availability

Allow requests to be served by healthy replicas according to the consistency guarantees of the system.

### Load Distribution

Distribute storage and request workloads across nodes to avoid overloading individual machines.

### Elasticity

Support adding and removing nodes without requiring a complete database rebuild.

### Local Storage Independence

Each storage node should retain its own local storage-engine implementation.

The distributed layer should not require the local storage engine to understand cluster-wide placement decisions.

### Consistency

Ensure that replicated data and distributed transactions follow explicitly defined consistency guarantees.

### Operational Visibility

Expose information about:

- Node health.
- Tablet ownership.
- Replica health.
- Replication lag.
- Partition distribution.
- Storage utilization.
- Rebalancing progress.
- Request routing.
- Transaction coordination.

### Non-Goals for the Initial Implementation

The initial distributed storage implementation does not need to provide:

- Automatic multi-region placement.
- Arbitrary cross-region transactions.
- Globally distributed SQL optimization.
- Multi-leader writes.
- Automatic schema evolution across heterogeneous nodes.
- Fully elastic online repartitioning.

These capabilities can be introduced after the core distributed architecture is established.

## Sharding

Sharding divides a dataset into independently managed subsets distributed across multiple storage nodes.

Each shard owns a portion of the overall dataset.

Instead of every node storing the entire database, individual nodes store only the shards assigned to them.

### Sharding Architecture

    Database
       |
       v
    Shard Map
       |
       +-------------------+
       |                   |
       v                   v
    Shard 0              Shard 1
       |                   |
       v                   v
    Node A               Node B
       |                   |
       v                   v
    Local Storage         Local Storage

Each shard is independently addressable and can have its own replication group.

### Shard Identity

Every shard must have a stable identifier.

Conceptual structure:

    ShardMetadata {
        shard_id
        database_id
        table_id
        partition_id
        partition_key_range
        placement_group_id
        schema_version
        generation
        status
    }

The shard identifier must remain stable across node failures and placement changes.

A shard's physical location may change without changing its logical identity.

### Shard Ownership

Each shard has an authoritative owner or replication group responsible for accepting writes.

In a leader-based replication architecture:

- One replica acts as the leader.
- Other replicas act as followers.
- Writes are coordinated through the leader.
- Committed updates are replicated according to the replication protocol.
- Leadership may change after failure.

The ownership model must prevent two nodes from independently accepting conflicting writes for the same shard.

### Shard Distribution

Shards can be distributed using:

- Range-based placement.
- Hash-based placement.
- Consistent hashing.
- Directory-based placement.

The selected strategy determines how data keys map to shards and how shards map to nodes.

### Shard Independence

Each shard should have independent:

- Storage files.
- WAL state.
- Replication state.
- Recovery state.
- Ownership metadata.
- Operational metrics.

This allows individual shards to be recovered, moved, or rebalanced without requiring the entire database to be unavailable.

### Hot Shards

A shard may receive significantly more requests than others.

Possible causes include:

- Highly skewed key distributions.
- Frequently accessed records.
- Monotonically increasing keys.
- Uneven partition sizes.
- Workloads concentrated on a small key range.

Mitigation strategies include:

- Splitting large shards.
- Splitting hot key ranges.
- Adding read replicas.
- Separating frequently accessed data.
- Changing the partitioning strategy.
- Introducing workload-aware placement.

Adding replicas can increase read capacity, but it does not automatically increase write capacity when writes remain serialized through a single shard leader.

## Partitioning

Partitioning divides a table or dataset into logical subsets according to a partitioning key.

Sharding determines how those subsets are distributed across the cluster.

Partitioning and sharding are related but distinct concepts.

A table may contain multiple partitions, and each partition may be divided into multiple shards or tablets.

### Partitioning Strategies

#### Range Partitioning

Data is divided into ranges of partition-key values.

Example:

    Partition 0: key < 1000
    Partition 1: 1000 <= key < 2000
    Partition 2: 2000 <= key < 3000
    Partition 3: key >= 3000

Advantages:

- Efficient range scans.
- Natural ordering.
- Good locality for range-based queries.
- Straightforward partition pruning.

Disadvantages:

- Uneven distribution when key ranges are skewed.
- Hot partitions for monotonically increasing keys.
- Requires splitting and boundary management.

Range partitioning is useful when queries frequently access contiguous key ranges.

#### Hash Partitioning

A hash function maps partition keys to logical partitions.

    partition_id = hash(partition_key) % partition_count

Advantages:

- Generally distributes keys more evenly.
- Avoids simple range-based hotspots.
- Provides predictable key-to-partition mapping.

Disadvantages:

- Range queries may require multiple partitions.
- Changing the partition count can remap many keys.
- Hash quality directly affects distribution.

#### List Partitioning

Data is assigned to partitions based on explicit key values.

Example:

    Partition 0: region IN ('APAC', 'SEA')
    Partition 1: region IN ('EMEA')
    Partition 2: region IN ('AMER')

This is useful when the partitioning dimension has a small, known set of values.

#### Composite Partitioning

Multiple partitioning dimensions can be combined.

For example:

    First Level: HASH(customer_id)
    Second Level: RANGE(created_at)

This allows the system to distribute customers across partitions while maintaining time-based organization within each partition.

### Partition Metadata

Conceptual structure:

    PartitionMetadata {
        partition_id
        table_id
        partitioning_strategy
        partition_key_definition
        lower_bound
        upper_bound
        generation
        status
    }

Partition metadata must be versioned because partition boundaries and ownership can change over time.

### Partition Pruning

The query layer should eliminate partitions that cannot contain the requested data.

For example:

    SELECT *
    FROM orders
    WHERE customer_id = 100
      AND created_at >= '2026-01-01';

If the partitioning scheme supports identifying relevant partitions from these predicates, the query router can avoid contacting unrelated nodes.

### Partition Splitting

A partition may grow beyond its configured size or become a workload hotspot.

The system should support splitting a partition into smaller partitions.

Example:

    Before:

        Partition P0
        Range: [0, 10000)

    After:

        Partition P1
        Range: [0, 5000)

        Partition P2
        Range: [5000, 10000)

The split must preserve:

- Record correctness.
- Index consistency.
- Replica consistency.
- Transaction visibility.
- Query routing correctness.
- Recovery correctness.

Partition splitting must be coordinated with concurrent writes so that no committed update is lost or duplicated.

### Partition Merging

Small or underutilized partitions may be merged to reduce metadata and operational overhead.

Merging requires validating that the partitions have compatible schemas, placement policies, and ownership state.

### Partition Metadata Consistency

Partition metadata must be authoritative and versioned.

A query router using an outdated partition map must not silently route writes to an obsolete owner.

The system should use metadata versions, ownership epochs, or equivalent fencing mechanisms to detect stale routing decisions.

## Consistent Hashing

Consistent hashing provides a mechanism for distributing keys across a changing set of storage nodes while minimizing key remapping when nodes are added or removed.

Traditional modulo-based hashing has a major limitation:

    node_id = hash(key) % node_count

Changing `node_count` can remap a large fraction of keys.

Consistent hashing reduces this disruption by mapping both keys and nodes onto a logical hash ring.

### Hash Ring

Each node is assigned one or more positions on the ring.

Each data key is hashed to a position on the same ring.

A key is assigned to the first node encountered while moving clockwise from the key's hash position.

Example:

    Hash Ring:

                 Node A
                   |
            .------+------.
          /                 \
         /                   \
    Node D                   Node B
         \                   /
          \                 /
            '------+------'
                   |
                 Node C

    Key K
      |
      v
    hash(K)
      |
      v
    First clockwise node
      |
      v
    Assigned owner

### Virtual Nodes

A physical node may own multiple virtual positions on the ring.

Instead of assigning one position per machine, the system creates multiple virtual nodes.

Example:

    Physical Node A:
        A-1
        A-2
        A-3
        A-4

    Physical Node B:
        B-1
        B-2
        B-3
        B-4

Virtual nodes improve distribution and reduce dependence on a single hash position.

They also provide finer-grained control over how much data is reassigned when cluster membership changes.

### Node Addition

When a new node joins:

1. Assign virtual positions to the node.
2. Identify the key ranges it becomes responsible for.
3. Transfer the corresponding data.
4. Synchronize required WAL or snapshot state.
5. Update the authoritative placement metadata.
6. Activate the new ownership assignment.

Only affected ranges should need to move.

### Node Removal

When a node is removed:

1. Mark the node unavailable for new placement.
2. Identify its affected ranges.
3. Select replacement owners.
4. Transfer data to replacement nodes.
5. Establish required replicas.
6. Publish updated placement metadata.
7. Remove obsolete ownership assignments.

### Consistent Hashing and Replica Placement

Consistent hashing can also identify successor nodes for replica placement.

For a primary owner, the system can select additional nodes while respecting placement constraints.

However, simply selecting the next nodes on the ring does not guarantee fault-domain independence.

Placement policies must additionally consider:

- Availability zones.
- Racks.
- Hosts.
- Regions.
- Node capacity.
- Existing replica distribution.

### Limitations

Consistent hashing is not a complete distributed metadata or consistency protocol.

It does not independently provide:

- Authoritative shard ownership.
- Safe ownership transfer.
- Replica synchronization.
- Failure detection.
- Consensus.
- Transaction coordination.
- Protection against stale routers.

MiniDB will use consistent hashing as a placement and distribution mechanism, while authoritative ownership and membership changes remain coordinated through cluster metadata and the replication layer.

## Replica Placement

Replica placement determines which storage nodes maintain copies of a shard or tablet.

The objective is to provide fault tolerance while balancing storage capacity, network traffic, and request load.

### Replication Factor

A replication factor defines the number of copies maintained for a shard.

For example:

    Replication Factor = 3

    Shard S1:
        Node A: Leader
        Node B: Follower
        Node C: Follower

The leader accepts writes, while followers maintain replicated copies.

The replication protocol determines when a write is considered committed.

A three-replica group typically requires a majority of two replicas for quorum-based decisions.

### Placement Constraints

Replica placement should consider failure domains.

A placement policy may require replicas to be distributed across:

- Different hosts.
- Different racks.
- Different availability zones.
- Different regions.

For example:

    Availability Zone A:
        Node A: Leader

    Availability Zone B:
        Node B: Follower

    Availability Zone C:
        Node C: Follower

This reduces the probability that a single infrastructure failure makes all replicas unavailable.

### Placement Metadata

Conceptual structure:

    ReplicaPlacement {
        shard_id
        replica_id
        node_id
        role
        availability_zone
        generation
        state
    }

Possible replica states:

- UNASSIGNED
- INITIALIZING
- CATCHING_UP
- ACTIVE
- DEGRADED
- RECOVERING
- REMOVING

### Placement Policies

The placement manager should support policies based on:

- Replication factor.
- Node capacity.
- Storage utilization.
- Network topology.
- Failure-domain constraints.
- Existing workload.
- Replica health.
- Data locality.

### Rebalancing

Rebalancing redistributes replicas when cluster membership or resource utilization changes.

Triggers include:

- Adding a node.
- Removing a node.
- Node failure.
- Uneven storage utilization.
- Hotspot detection.
- Changing replication factor.
- Changing placement constraints.

Rebalancing should be incremental to avoid overwhelming the cluster.

### Replica Promotion

When a leader becomes unavailable, a healthy replica may become the new leader through the consensus protocol.

The promoted replica must satisfy the replication protocol's safety requirements.

Placement metadata must be updated after leadership changes without creating competing authoritative owners.

### Placement and Consistency

Replica placement is not equivalent to replica consistency.

Placement determines where replicas reside.

Replication determines how their state is synchronized.

The replication and consensus protocols remain responsible for ensuring that committed writes are not lost during leadership changes, subject to their durability assumptions.

## Tablet Management

A tablet is a manageable unit of distributed storage.

It represents a portion of a table or dataset that can be independently placed, replicated, moved, split, merged, and recovered.

Tablets provide a useful abstraction between logical partitioning and physical storage.

### Tablet Architecture

    Table
      |
      v
    Partition
      |
      v
    Tablet Map
      |
      +------------------+
      |                  |
      v                  v
    Tablet 1           Tablet 2
      |                  |
      v                  v
    Replica Group       Replica Group
      |                  |
      v                  v
    Storage Nodes       Storage Nodes

### Tablet Metadata

Conceptual structure:

    TabletMetadata {
        tablet_id
        table_id
        partition_id
        key_range
        generation
        leader_node_id
        replica_nodes
        schema_version
        state
        creation_timestamp
        last_split_timestamp
    }

Tablet metadata must be maintained in an authoritative metadata service.

### Tablet Lifecycle

A tablet may transition through the following states:

    UNASSIGNED
        |
        v
    CREATING
        |
        v
    INITIALIZING
        |
        v
    REPLICATING
        |
        v
    ACTIVE
        |
        +--------------------+
        |                    |
        v                    v
    SPLITTING             MIGRATING
        |                    |
        v                    v
    ACTIVE                 ACTIVE

Other lifecycle states include:

- RECOVERING
- DEGRADED
- MERGING
- DELETING
- FAILED

State transitions must be validated so that concurrent management operations cannot create conflicting tablet ownership.

### Tablet Creation

Tablet creation involves:

1. Allocate a tablet identifier.
2. Select a partition or key range.
3. Choose an initial storage node.
4. Initialize local storage.
5. Initialize WAL and recovery metadata.
6. Establish the replication group.
7. Synchronize required replicas.
8. Publish the tablet as active.

The tablet should not become available for normal writes until its ownership and replication requirements are satisfied.

### Tablet Splitting

A large tablet can be divided into smaller tablets.

Example:

    Before:

        Tablet T1
        Range: [0, 10000)

    After:

        Tablet T2
        Range: [0, 5000)

        Tablet T3
        Range: [5000, 10000)

A split operation must establish a precise boundary between the parent and child tablets.

The transition must account for concurrent writes and in-flight transactions.

A possible workflow:

1. Mark the parent tablet as splitting.
2. Establish a consistent split boundary.
3. Create child tablet metadata.
4. Copy or reorganize records into child tablets.
5. Synchronize child replicas.
6. Record the split completion.
7. Publish the new tablet map.
8. Retire the parent tablet after all required references are safe.

The implementation must ensure that every committed record belongs to exactly one authoritative child tablet after the split.

### Tablet Migration

Tablet migration moves ownership or physical data from one node to another.

Example:

    Before:

        Tablet T1
            Leader: Node A
            Follower: Node B

    After:

        Tablet T1
            Leader: Node C
            Follower: Node B

Migration may involve:

- Snapshot transfer.
- WAL catch-up.
- Replica initialization.
- Leadership transfer.
- Metadata updates.
- Old replica cleanup.

A migration should not require an application-visible interruption when the replication protocol and ownership-transfer mechanism can safely coordinate the transition.

### Tablet Merging

Two adjacent or compatible tablets may be merged to reduce management overhead.

The operation must preserve:

- Key ordering.
- Index correctness.
- Transaction visibility.
- Replica consistency.
- Ownership metadata.
- Recovery boundaries.

### Tablet Management and Recovery

Tablets should be independently recoverable.

A node restarting after failure should be able to:

1. Load local tablet metadata.
2. Identify its assigned replicas.
3. Recover local storage using WAL.
4. Reconcile its replication state.
5. Catch up from an authoritative replica if necessary.
6. Rejoin the cluster after validation.

A local recovery completion does not automatically mean the replica is eligible to serve reads or become leader. Replication state and ownership must also be validated.

## Query Routing

Query routing determines which storage nodes must execute a query.

The query router uses partition metadata, shard mappings, tablet ownership, and replica health to select destinations.

### Query Router Architecture

    Client
      |
      v
    Query Router
      |
      v
    Parse Query
      |
      v
    Extract Predicates
      |
      v
    Partition Pruning
      |
      v
    Resolve Tablet Mapping
      |
      v
    Select Replica
      |
      v
    Dispatch Request
      |
      v
    Aggregate Results
      |
      v
    Return Result

### Routing Metadata

The query router requires:

- Table partition definitions.
- Partition key definitions.
- Shard mappings.
- Tablet locations.
- Leader and follower information.
- Replica health.
- Metadata versions.
- Schema versions.

The router should cache metadata to avoid consulting the metadata service for every request.

Cached metadata must be versioned and validated when routing decisions fail.

### Single-Partition Queries

A query targeting one partition should be routed directly to the responsible tablet.

Example:

    SELECT *
    FROM users
    WHERE user_id = 100;

If `user_id` determines the partition, the router can identify the relevant tablet without contacting other nodes.

### Multi-Partition Queries

Queries spanning multiple partitions require distributed execution.

Example:

    SELECT *
    FROM orders
    WHERE created_at >= '2026-01-01';

The router identifies the relevant partitions and dispatches requests to the corresponding tablets.

Partial results are collected and combined before returning the result.

### Scatter-Gather Queries

A query without a usable partition predicate may require execution across multiple shards.

    Query Router
         |
         +----------+----------+
         |          |          |
         v          v          v
       Node A     Node B     Node C
         |          |          |
         v          v          v
       Result A   Result B   Result C
         |          |          |
         +----------+----------+
                    |
                    v
               Aggregation
                    |
                    v
                 Client

The query execution layer may need to perform:

- Distributed filtering.
- Partial aggregation.
- Sorting.
- Limit and offset.
- Duplicate elimination.
- Result merging.

Scatter-gather operations can be expensive and should be avoided when partition pruning is possible.

### Read Routing

Read requests may be routed to:

- The leader.
- A follower with sufficiently fresh state.
- A replica satisfying the requested consistency level.

Leader reads provide a straightforward consistency model.

Follower reads may reduce load and improve locality, but can return stale data depending on replication progress and the read protocol.

### Write Routing

Writes must be routed to the authoritative leader for the relevant shard.

If a router contacts a follower, the follower should reject the write or redirect it to the current leader.

Stale ownership metadata must not allow a former leader to accept writes after losing authority.

### Stale Routing Metadata

A query router may hold an outdated tablet map.

The system should handle this through:

- Metadata versions.
- Ownership epochs.
- Leader redirects.
- Retry with refreshed metadata.
- Fencing of stale owners.

Retries must be designed carefully because a client may not know whether a write was committed before a network failure.

Idempotency keys or transaction identifiers may be required to avoid duplicate logical operations.

### Distributed Query Execution

The query router should remain separate from the local query execution engine.

The local engine executes operations against local tablets.

The distributed execution layer coordinates remote execution and combines partial results.

This separation allows MiniDB to evolve local query processing independently from distributed query planning.

## Distributed Transactions

Distributed transactions coordinate atomic operations spanning multiple shards or tablets.

A transaction that touches only one shard can use the local transaction manager.

A transaction that modifies multiple shards requires coordination across the participating transaction managers.

### Transaction Architecture

    Client
      |
      v
    Distributed Transaction Coordinator
      |
      +-------------------+
      |                   |
      v                   v
    Participant A       Participant B
      |                   |
      v                   v
    Local WAL            Local WAL
      |                   |
      v                   v
    Local Transaction    Local Transaction
    Manager              Manager

The coordinator manages the global transaction.

Each participant manages its local transaction state and durable records.

### Transaction Identity

A distributed transaction requires a globally unique transaction identifier.

Conceptual structure:

    DistributedTransaction {
        global_transaction_id
        coordinator_id
        participant_shards
        isolation_level
        state
        start_timestamp
        commit_timestamp
    }

Each participant also maintains its local transaction identifier.

The global identifier links participant state to the coordinator's transaction.

### Two-Phase Commit

Two-Phase Commit (2PC) is a coordination protocol that allows participants to agree on whether a distributed transaction can commit.

It consists of two phases.

#### Phase 1: Prepare

The coordinator sends PREPARE to every participant.

Each participant:

1. Validates the transaction.
2. Checks required constraints.
3. Ensures its modifications can be committed.
4. Persists the prepared state in its WAL.
5. Retains required locks or transactional resources.
6. Responds with YES or NO.

A participant that votes YES must retain enough durable information to complete the transaction after restart.

#### Phase 2: Commit or Abort

If every participant votes YES:

    Coordinator
        |
        v
    COMMIT Decision
        |
        +-------------------+
        |                   |
        v                   v
    Participant A       Participant B
        |                   |
        v                   v
    Commit Local         Commit Local
    Transaction          Transaction

If any participant votes NO or the transaction cannot be prepared:

    Coordinator
        |
        v
    ABORT Decision
        |
        +-------------------+
        |                   |
        v                   v
    Participant A       Participant B
        |                   |
        v                   v
    Abort Local           Abort Local
    Transaction           Transaction

The coordinator must persist its decision durably before sending the corresponding final decision to participants.

### 2PC Failure Handling

A participant may crash after preparing but before receiving the final decision.

After restart, it must recover its prepared transaction state.

It cannot independently assume that the transaction should abort.

It must recover the coordinator's decision or participate in a defined transaction-resolution protocol.

A coordinator crash after sending PREPARE but before persisting or distributing the final decision can leave participants blocked while waiting for resolution.

2PC therefore introduces blocking behavior under certain failures.

### Distributed Commit and WAL

Every participant must persist sufficient WAL state to recover its local transaction.

The coordinator must persist:

- Global transaction identity.
- Participant list.
- Prepare responses.
- Final commit or abort decision.
- Transaction completion state.

The transaction coordinator and local recovery managers must cooperate during restart.

### Isolation

Atomic commit does not automatically provide distributed serializable isolation.

The system must separately define how transactions coordinate concurrent reads and writes.

Possible approaches include:

- Two-phase locking across participants.
- MVCC with distributed timestamps.
- Snapshot isolation with distributed conflict detection.
- Serializable transaction coordination.

The chosen isolation model must define how snapshots are established and how conflicts are detected across shards.

### Distributed Deadlocks

Transactions may acquire locks on different shards in conflicting orders.

For example:

    Transaction T1:
        Lock Shard A
        Wait for Shard B

    Transaction T2:
        Lock Shard B
        Wait for Shard A

This creates a distributed deadlock.

Possible approaches include:

- Global wait-for graph.
- Distributed deadlock detection.
- Deterministic lock ordering.
- Transaction timeouts.
- Deadlock prevention policies.

### Initial Scope

The initial distributed transaction implementation should prioritize:

- Single-shard transactions.
- Transaction identity propagation.
- Local durability.
- Cross-shard transaction participant interfaces.
- Durable prepare and commit records.

Full distributed atomic commit and stronger isolation guarantees can be introduced incrementally.

## Failure Recovery

Distributed failure recovery restores the cluster to a valid operating state after node, network, storage, or replication failures.

It builds on local crash recovery defined in `recovery.md` and replica coordination defined in `replication.md` and `raft.md`.

### Failure Types

The system must account for:

- Process crashes.
- Machine crashes.
- Disk failures.
- Network partitions.
- Network delays.
- Replica lag.
- Leader failures.
- Metadata service failures.
- Corrupted local storage.
- Partial tablet migration.
- Incomplete replica initialization.
- Distributed transaction coordinator failures.

### Node Failure Detection

The cluster manager monitors node availability through periodic health checks or heartbeats.

A node that stops responding may be suspected of failure.

Failure detection is inherently imperfect in an asynchronous network.

A slow node or delayed network message does not necessarily indicate permanent failure.

Therefore, failure suspicion must not directly authorize conflicting writes.

Leadership and ownership changes must be coordinated through the consensus and fencing mechanisms.

### Leader Failure

When a shard leader becomes unavailable:

1. Replicas detect the loss of leader communication.
2. The replication protocol initiates leader election.
3. A valid replica becomes the new leader.
4. The replication group establishes its new committed state.
5. Cluster metadata is updated.
6. Query routers refresh stale ownership information.
7. Writes resume when the new leader is ready.

The system must prevent the old leader from continuing to serve authoritative writes after losing leadership.

### Follower Failure

When a follower becomes unavailable:

1. The cluster detects the missing replica.
2. The shard continues according to its remaining quorum and availability state.
3. The failed replica is marked unavailable.
4. A replacement may be selected.
5. The replacement receives a snapshot or required log entries.
6. The replica catches up.
7. The replica becomes active after validation.

The cluster should expose degraded replication health while the replication factor is not satisfied.

### Node Restart

A restarted node must:

1. Load local cluster and tablet metadata.
2. Validate local storage.
3. Run local WAL recovery.
4. Determine its replication position.
5. Reconcile local state with the authoritative replica group.
6. Catch up using WAL or snapshot transfer.
7. Rejoin as a healthy replica.

A node must not become an authoritative leader solely because its local storage recovered successfully.

### Replica Catch-Up

A replica may be behind the leader.

The recovery mechanism determines whether the replica can catch up using retained log entries or requires a full snapshot.

    Replica Restart
          |
          v
    Compare Replication Position
          |
          v
    Required WAL Available?
          |
       +--+--+
       |     |
      Yes    No
       |     |
       v     v
    WAL     Snapshot
    Replay  Transfer
       |     |
       v     v
    Apply Missing Entries
          |
          v
    Validate State
          |
          v
    Replica Active

### Network Partition

A network partition can isolate nodes while leaving them operational locally.

The system must prevent multiple partitions from independently accepting conflicting authoritative writes.

For a quorum-based consensus group:

- The majority partition can continue making progress.
- The minority partition cannot commit new entries.
- A former leader that loses quorum must stop accepting authoritative writes.
- Replicas must reconcile state after connectivity is restored.

The exact availability behavior depends on the replication and consistency protocol.

### Tablet Recovery

A tablet whose leader or replica fails may need to be reassigned.

The recovery manager should coordinate:

- Local recovery.
- Replica synchronization.
- Leadership transfer.
- Placement updates.
- Routing metadata refresh.
- Validation of ownership generation.

### Distributed Transaction Recovery

Distributed transaction recovery must coordinate participant state with the durable coordinator decision.

After restart:

- Committed transactions must complete on all participants.
- Aborted transactions must roll back on all participants.
- Prepared transactions with unresolved decisions must remain recoverable.
- Participants must not independently commit an undecided transaction.

The recovery process must preserve atomicity even when participants restart at different times.

### Recovery and Rebalancing

Failure recovery and normal rebalancing should use compatible ownership-transfer mechanisms.

A replacement replica should not become authoritative until it has reached the required recovery and replication state.

Metadata changes must be published in an order that prevents clients from routing writes to stale or incomplete owners.

## Design Decisions

### 1. Separate Logical Distribution from Physical Placement

Partitioning and sharding define the logical organization of data.

Tablet placement determines where the data is physically hosted.

This separation allows MiniDB to move tablets without changing the logical identity of their data.

### 2. Use Tablets as the Primary Management Unit

Tablets provide a consistent unit for:

- Replica placement.
- Ownership.
- Migration.
- Splitting.
- Merging.
- Recovery.
- Monitoring.

This avoids requiring cluster management to operate directly on individual database pages.

### 3. Support Multiple Partitioning Strategies

MiniDB should support range and hash partitioning as primary strategies.

List and composite partitioning can be added as the query and metadata layers mature.

Partitioning strategy should be explicit in table metadata.

### 4. Use Consistent Hashing for Placement

Consistent hashing with virtual nodes will be used to reduce key remapping when cluster membership changes.

It will not replace authoritative shard metadata or consensus-based ownership coordination.

### 5. Use Leader-Based Replication Initially

Each shard or tablet replication group will use a single authoritative leader.

Writes are routed to the leader.

Followers replicate the committed log according to the protocol defined in `replication.md`.

This simplifies write ownership and conflict handling.

### 6. Separate Cluster Metadata from User Data

Cluster metadata includes:

- Node membership.
- Partition definitions.
- Shard mappings.
- Tablet locations.
- Replica placement.
- Ownership generations.
- Schema versions.

This metadata should be managed independently from ordinary user records.

The metadata service itself must be fault tolerant and protected against conflicting updates.

### 7. Use Versioned Ownership Metadata

Every ownership or placement transition should advance a generation or epoch.

Requests carrying stale metadata can be rejected or redirected.

Ownership epochs help prevent stale nodes and stale query routers from acting on obsolete assignments.

### 8. Keep Query Routing Separate from Local Execution

The distributed query router determines where work should execute.

The local query engine executes the request against local storage.

Distributed execution coordinates remote operations and combines results.

This allows the local execution engine to remain independent from cluster topology.

### 9. Start with Single-Shard Transactions

Single-shard transactions should use the existing local transaction and recovery architecture.

Cross-shard transaction coordination will be introduced through a distributed transaction coordinator.

Two-Phase Commit is the initial candidate for atomic cross-shard commit.

Its blocking behavior and failure-resolution requirements must be explicitly addressed.

### 10. Reuse Existing Recovery and Replication Architecture

Distributed storage should build on:

- `recovery.md` for local crash recovery.
- `replication.md` for leader-follower replication.
- `raft.md` for consensus and leadership.
- `transactions.md` for local transaction semantics.
- `mvcc.md` for version visibility and snapshot semantics.

The distributed layer should coordinate these components rather than duplicate their responsibilities.

### 11. Initial Implementation Phases

**Stage 1 — Cluster Foundations**

- Define node identity.
- Define cluster membership metadata.
- Define shard and tablet identifiers.
- Define cluster metadata serialization.
- Introduce node health reporting.

**Stage 2 — Partitioning and Sharding**

- Define partition metadata.
- Implement range partitioning.
- Implement hash partitioning.
- Implement key-to-partition mapping.
- Implement shard metadata.
- Add partition pruning support.

**Stage 3 — Placement**

- Implement consistent hashing.
- Add virtual nodes.
- Implement replica placement policies.
- Add failure-domain metadata.
- Implement ownership generations.

**Stage 4 — Tablet Management**

- Implement tablet creation.
- Implement tablet state transitions.
- Implement tablet-to-node assignment.
- Implement snapshot-based tablet initialization.
- Add WAL-based replica catch-up.
- Implement basic tablet migration.

**Stage 5 — Distributed Query Routing**

- Implement query router.
- Add partition-aware routing.
- Add leader-based write routing.
- Add configurable read routing.
- Implement stale metadata handling.
- Implement scatter-gather execution.

**Stage 6 — Failure Recovery**

- Add node failure detection.
- Integrate Raft leader election.
- Implement replica catch-up.
- Implement tablet reassignment.
- Add recovery-aware routing.
- Add failure injection tests.

**Stage 7 — Distributed Transactions**

- Implement global transaction identity.
- Implement participant registration.
- Add durable prepare records.
- Implement coordinator decision logging.
- Implement commit and abort propagation.
- Add distributed transaction recovery.
- Integrate distributed isolation and deadlock handling.

**Stage 8 — Elasticity and Advanced Operations**

- Implement tablet splitting.
- Implement tablet merging.
- Add workload-aware rebalancing.
- Add hot-shard detection.
- Add automated replica replacement.
- Add operational metrics and observability.

### 12. Suggested Module Organization

    distributed/
        cluster/
            cluster_manager.h
            cluster_manager.cpp
            node_registry.h
            node_registry.cpp
            cluster_metadata.h
            cluster_metadata.cpp

        partition/
            partition_manager.h
            partition_manager.cpp
            partition_metadata.h
            partition_metadata.cpp
            partitioner.h

        sharding/
            shard_manager.h
            shard_manager.cpp
            shard_map.h
            shard_map.cpp

        placement/
            placement_manager.h
            placement_manager.cpp
            consistent_hashing.h
            consistent_hashing.cpp
            replica_placement.h
            replica_placement.cpp

        tablet/
            tablet_manager.h
            tablet_manager.cpp
            tablet_metadata.h
            tablet_metadata.cpp
            tablet_migration.h
            tablet_migration.cpp
            tablet_splitter.h
            tablet_splitter.cpp

        routing/
            query_router.h
            query_router.cpp
            routing_metadata.h
            routing_metadata.cpp

        transaction/
            distributed_transaction.h
            distributed_transaction.cpp
            transaction_coordinator.h
            transaction_coordinator.cpp
            transaction_participant.h
            transaction_participant.cpp

        recovery/
            distributed_recovery.h
            distributed_recovery.cpp
            replica_recovery.h
            replica_recovery.cpp

The module organization is illustrative. Interfaces should be introduced as responsibilities become implementable rather than creating empty abstractions prematurely.

### 13. Testing Strategy

Distributed storage testing must validate correctness under both normal operation and failures.

**Partitioning and Sharding**

- Key-to-partition mapping.
- Range boundary correctness.
- Hash distribution.
- Partition pruning.
- Hot partition behavior.
- Partition splitting and merging.

**Consistent Hashing**

- Stable mapping with unchanged membership.
- Key redistribution after node addition.
- Key redistribution after node removal.
- Virtual-node distribution.
- Failure-domain-aware replica selection.

**Replica Placement**

- Replication factor enforcement.
- Rack and availability-zone separation.
- Capacity-aware placement.
- Replica replacement.
- Degraded cluster behavior.

**Tablet Management**

- Tablet creation.
- Tablet migration.
- Tablet splitting.
- Tablet merging.
- Interrupted migration.
- Recovery during splitting.
- Stale ownership metadata.

**Query Routing**

- Single-partition routing.
- Multi-partition routing.
- Scatter-gather queries.
- Leader-based writes.
- Follower reads.
- Stale router metadata.
- Retry after leader changes.

**Distributed Transactions**

- Single-shard commit.
- Cross-shard commit.
- Cross-shard abort.
- Participant failure during prepare.
- Coordinator failure.
- Recovery of prepared transactions.
- Distributed deadlock scenarios.

**Failure Recovery**

- Leader crash.
- Follower crash.
- Node restart.
- Network partition.
- Replica lag.
- Snapshot transfer.
- WAL catch-up.
- Tablet reassignment.
- Failure during rebalancing.

## Open Questions

### Cluster Architecture

- Will MiniDB use a centralized metadata service or a consensus-backed distributed metadata group?
- Will cluster membership itself be managed through Raft?
- How will node identities survive process restarts?
- How will cluster configuration changes be versioned?
- How will metadata snapshots and recovery work?
- Can metadata operations become a bottleneck?

### Sharding and Partitioning

- Will shards and tablets have a one-to-one relationship initially?
- Should partitioning be selected at table creation or support online changes?
- Will range and hash partitioning be the only initial strategies?
- How will composite partitioning be represented?
- How will partition boundaries be stored and versioned?
- How will partition splits coordinate with active transactions?
- How will hot partitions be detected and mitigated?

### Consistent Hashing

- Which hash function will be used?
- How many virtual nodes should each physical node own?
- How will virtual-node weights account for different node capacities?
- Should consistent hashing map keys directly to nodes or map logical ranges to tablet identifiers?
- How will hash-ring changes be coordinated?
- How will consistent hashing interact with range partitioning?
- Should placement be deterministic across cluster restarts?

### Replica Placement

- What is the default replication factor?
- Will placement be availability-zone aware?
- How will replica placement account for heterogeneous hardware?
- How will the system prevent replicas from sharing a failure domain?
- How will replica replacement be scheduled?
- How will rebalancing avoid excessive network and disk I/O?
- How will the system behave when the cluster cannot satisfy the requested replication factor?

### Tablet Management

- What is the initial tablet size?
- Should tablets represent fixed key ranges or independently managed storage files?
- How will tablet splitting establish a consistent boundary?
- How will tablet merging handle concurrent writes?
- How will migrations transfer snapshots and WAL?
- How will interrupted migrations be resumed?
- How will old tablet generations be retired safely?
- How will tablet metadata be replicated and recovered?

### Query Routing

- Will the query router be embedded in the database server or implemented as a separate service?
- How will routing metadata be cached?
- How will stale metadata be invalidated?
- Will clients connect directly to storage nodes?
- How will read consistency levels be exposed?
- How will follower read freshness be evaluated?
- How will scatter-gather queries be optimized?
- How will distributed joins and aggregations be executed?
- How will duplicate requests be handled after network timeouts?

### Distributed Transactions

- Will MiniDB use Two-Phase Commit for cross-shard transactions?
- How will coordinator state be replicated?
- How will prepared transactions be resolved after coordinator failure?
- How will distributed transaction timeouts work?
- Will strict two-phase locking be supported across shards?
- How will distributed snapshots be established?
- How will MVCC timestamps be coordinated?
- How will distributed deadlocks be detected?
- What is the policy for transactions involving unavailable participants?

### Failure Recovery

- How will failure detection distinguish slow nodes from failed nodes?
- How will ownership epochs fence former leaders?
- How will local recovery coordinate with replica catch-up?
- When should a replica use WAL replay versus snapshot transfer?
- How will the system handle simultaneous failures?
- How will network partitions affect reads and writes?
- How will metadata recovery work if the metadata quorum is unavailable?
- How will recovery coordinate with tablet migration and rebalancing?

### Consistency and Scalability

- Will the default consistency model be linearizable writes with configurable follower reads?
- Which operations require quorum confirmation?
- How will cross-shard consistency differ from single-shard consistency?
- Will multi-region deployments be supported?
- How will clock synchronization affect distributed transactions?
- How will the system handle skewed workloads?
- How will distributed query planning account for network latency and data locality?

### Observability and Operations

- Which cluster-level metrics will be exposed?
- How will shard and tablet health be inspected?
- How will replica lag be measured?
- How will rebalancing progress be monitored?
- How will hot shards be identified?
- How will operators trigger manual tablet migration?
- How will cluster membership changes be audited?
- What operational controls are needed for safe node maintenance?

### Final Architectural Principle

Distributed storage must preserve the correctness guarantees of the local storage engine while introducing data ownership, placement, replication, routing, and coordination across multiple nodes.

Partitioning determines how data is divided. Sharding determines how logical data units are distributed. Consistent hashing assists with placement. Tablets provide manageable units of storage. Replication maintains copies. Consensus coordinates authoritative ownership. Query routing directs requests. Distributed transactions coordinate atomic operations across shards.

These responsibilities must remain clearly separated, with explicit contracts between the distributed control plane and the local storage engine.

A distributed database is not simply a collection of independent storage nodes. It is a coordinated system in which every ownership transition, replica update, transaction decision, and recovery operation must preserve data correctness under concurrency and failure.