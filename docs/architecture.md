# Architecture

## Overview

MiniDB is a modular database system designed to evolve from a single-node storage engine into a transactional, replicated, and distributed database.

The architecture separates storage management, indexing, transaction processing, durability, replication, consensus, and distributed coordination into independently defined components.

The system is built around a common storage infrastructure that supports multiple storage engines, including a B+Tree-based storage engine and an LSM-based storage engine.

Higher-level components provide transactional semantics, crash recovery, replication, consensus, and distributed data management.

The architecture follows a layered design:

- **Common Infrastructure:** Provides disk I/O, page management, buffer pooling, WAL, serialization, and shared storage abstractions.
- **BTree Engine:** Provides page-oriented indexing and storage using B+Tree structures.
- **LSM Engine:** Provides write-optimized storage using MemTables, SSTables, and compaction.
- **Transaction Layer:** Coordinates transactions, locking, isolation, MVCC, commit, and rollback.
- **Replication Layer:** Maintains copies of database state across replicas.
- **Consensus Layer:** Coordinates leadership, log replication, membership, and authoritative decisions.
- **Distributed Storage Layer:** Manages partitioning, sharding, tablet placement, query routing, and distributed transactions.

### Architectural Principles

**Modularity**

Each component should have a well-defined responsibility and communicate through explicit interfaces.

**Separation of Concerns**

Storage structures, transaction semantics, durability, and distributed coordination should remain logically independent.

**Durability**

Committed transactions must survive crashes through WAL-based recovery.

**Consistency**

Storage structures and transactional state must remain valid across normal execution, crashes, and recovery.

**Extensibility**

The architecture should support additional storage engines, indexing strategies, and distributed capabilities without requiring a complete redesign.

**Observability**

Internal operations should expose sufficient metrics and diagnostics for debugging and operational analysis.

**Incremental Implementation**

The architecture should allow individual subsystems to be implemented and validated independently before integrating them into the complete database.

## Goals

### Storage Engine Flexibility

Support multiple storage architectures:

- B+Tree-based storage.
- LSM-based storage.
- Page-oriented storage management.
- Multiple index structures.
- Future vector and full-text indexing.

### Transactional Processing

Provide:

- ACID transactions.
- Shared and exclusive locking.
- Deadlock detection.
- MVCC.
- Snapshot isolation.
- Read committed and repeatable read.
- Commit and rollback.
- Distributed transaction coordination.

### Crash Recovery

Provide:

- Write-Ahead Logging.
- Page-oriented REDO.
- Transaction-oriented UNDO.
- Compensation Log Records.
- Checkpointing.
- Restartable recovery.
- Storage-engine structural recovery.

### High Availability

Support:

- Leader-follower replication.
- Replica synchronization.
- Replica recovery.
- Leader election.
- Failure detection.
- Failover.
- Consistency guarantees.

### Distributed Storage

Support:

- Horizontal partitioning.
- Sharding.
- Consistent hashing.
- Replica placement.
- Tablet management.
- Query routing.
- Distributed transactions.
- Automatic rebalancing.

### Performance and Scalability

Provide architectural foundations for:

- Efficient page caching.
- Sequential WAL writes.
- Concurrent transaction processing.
- Background compaction.
- Parallel query execution.
- Distributed request routing.
- Workload-aware storage placement.

### Maintainability

Keep storage, transaction, and distributed subsystems independently testable and observable.

## High Level Architecture

MiniDB is organized into three major architectural layers:

1. Storage and common infrastructure.
2. Transactional and durability services.
3. Replication and distributed coordination.

### System Architecture

    +------------------------------------------------------+
    |                    Client Interface                  |
    |                                                      |
    |       KV API / SQL Interface / Query Interface       |
    +----------------------------+-------------------------+
                                 |
                                 v
    +------------------------------------------------------+
    |                  Query Processing                    |
    |                                                      |
    | Parser | Planner | Optimizer | Executor              |
    +----------------------------+-------------------------+
                                 |
                                 v
    +------------------------------------------------------+
    |                   Transaction Layer                  |
    |                                                      |
    | Transaction Manager                                  |
    | Lock Manager                                         |
    | MVCC / Snapshot Manager                              |
    | Commit / Abort                                       |
    +----------------------------+-------------------------+
                                 |
                                 v
    +------------------------------------------------------+
    |                    Storage Layer                     |
    |                                                      |
    |       +----------------+    +----------------+       |
    |       |  BTree Engine  |    |   LSM Engine   |       |
    |       +----------------+    +----------------+       |
    |                                                      |
    |       Indexes / Data Access / Storage Abstraction    |
    +----------------------------+-------------------------+
                                 |
                                 v
    +------------------------------------------------------+
    |                 Common Infrastructure                |
    |                                                      |
    | Buffer Pool | Page Manager | Disk Manager            |
    | WAL Manager | Checkpoint Manager | Serialization     |
    +----------------------------+-------------------------+
                                 |
                                 v
    +------------------------------------------------------+
    |                  Persistent Storage                  |
    |                                                      |
    | Database Files | Index Files | WAL | SSTables        |
    +------------------------------------------------------+


    +------------------------------------------------------+
    |                Replication Layer                     |
    |                                                      |
    | Leader / Follower | WAL Shipping | Replica Recovery  |
    +----------------------------+-------------------------+
                                 |
                                 v
    +------------------------------------------------------+
    |                 Consensus Layer                     |
    |                                                      |
    | Raft | Leader Election | Log Replication             |
    | Membership Changes | Failure Handling               |
    +----------------------------+-------------------------+
                                 |
                                 v
    +------------------------------------------------------+
    |              Distributed Storage Layer               |
    |                                                      |
    | Sharding | Partitioning | Consistent Hashing         |
    | Replica Placement | Tablet Management               |
    | Query Routing | Distributed Transactions            |
    +------------------------------------------------------+

The diagram represents logical responsibilities. Replication, consensus, and distributed storage interact with the storage and transaction layers rather than forming a strictly sequential execution pipeline.

### Single-Node and Distributed Deployment

The same local storage engine should operate in both deployment models.

**Single-node deployment**

    Client
      |
      v
    Transaction Layer
      |
      v
    Storage Engine
      |
      v
    Common Infrastructure
      |
      v
    Local Disk

**Distributed deployment**

    Client
      |
      v
    Query Router
      |
      v
    Distributed Storage
      |
      v
    Shard / Tablet
      |
      v
    Replication Group
      |
      v
    Local Storage Engine
      |
      v
    Common Infrastructure
      |
      v
    Local Disk

This separation allows the local storage engine to remain independent of cluster topology.

## Core Components

### Common Infrastructure

Common infrastructure provides the foundational services used by all storage engines.

It is responsible for physical storage access, page management, memory caching, WAL persistence, and serialization.

The infrastructure should not depend on B+Tree-specific or LSM-specific behavior.

#### Disk Manager

The Disk Manager provides low-level persistent storage access.

Responsibilities:

- Create and open database files.
- Read fixed-size pages.
- Write fixed-size pages.
- Allocate and manage file storage.
- Flush persistent data.
- Report I/O errors.
- Provide storage primitives to higher-level components.

The initial implementation uses 4 KB pages.

The Disk Manager treats page contents as opaque bytes and does not interpret storage-engine-specific structures.

#### Page Manager

The Page Manager manages database page allocation and metadata.

Responsibilities:

- Allocate pages.
- Deallocate pages.
- Track allocated pages.
- Validate page identifiers.
- Persist allocation metadata.
- Maintain page lifecycle information.

Page 0 is reserved for database metadata, and data pages begin at page 1.

#### Buffer Pool Manager

The Buffer Pool Manager manages a bounded in-memory cache of database pages.

Responsibilities:

- Fetch pages from storage.
- Maintain page residency.
- Track pin counts.
- Track dirty pages.
- Evict eligible pages.
- Flush dirty pages.
- Coordinate page access with storage components.

The buffer pool provides a shared caching abstraction for page-oriented storage engines.

Future extensions include:

- Concurrent page access.
- PageGuard.
- Latch management.
- Advanced replacement policies.
- Background flushing.
- I/O scheduling.

#### WAL Manager

The WAL Manager provides durable logging.

Responsibilities:

- Append log records.
- Assign LSNs.
- Serialize log records.
- Flush WAL to persistent storage.
- Read log records during recovery.
- Maintain WAL segment metadata.
- Support transaction and storage-engine log records.

The WAL Manager provides the durability foundation for transactions and crash recovery.

#### Checkpoint Manager

The Checkpoint Manager periodically records recovery metadata.

Responsibilities:

- Capture checkpoint state.
- Record active transactions.
- Track dirty pages.
- Establish safe recovery boundaries.
- Coordinate WAL retention.
- Reduce startup recovery work.

#### Serialization Layer

The serialization layer defines stable persistent representations.

Responsibilities:

- Encode and decode page metadata.
- Serialize WAL records.
- Serialize storage-engine metadata.
- Handle explicit byte ordering.
- Validate persistent structures.
- Support format versioning.

Persistent data must not depend on compiler-specific C++ structure layouts or raw structure writes.

#### Common Infrastructure Architecture

    Storage Engine
          |
          v
    Storage Interfaces
          |
          +-------------------------------+
          |               |               |
          v               v               v
      Buffer Pool     Page Manager     WAL Manager
          |               |               |
          v               v               v
      Disk Manager    Disk Manager     WAL Storage
          |
          v
      Database Files

The actual dependency direction should be enforced through interfaces so that higher-level components do not bypass storage abstractions unnecessarily.

### BTree Engine

The BTree Engine provides page-oriented storage and indexing using B+Tree structures.

It is designed for efficient point lookups, ordered traversal, and range scans.

The engine uses the common page and buffer pool infrastructure.

#### Architecture

    BPlusTree
        |
        v
    BPlusTreeNode
        |
        v
    IndexPageManager
        |
        v
    BufferPoolManager
        |
        v
    DiskManager

#### Responsibilities

- Search keys.
- Insert keys.
- Maintain sorted entries.
- Split leaf nodes.
- Split internal nodes.
- Propagate separator keys.
- Split the root.
- Perform range scans.
- Maintain sibling links.
- Manage index page allocation.

#### Page Structure

The B+Tree uses explicit page formats.

Conceptual node metadata:

    BTreeNodeHeader {
        page_id
        parent_page_id
        node_type
        key_count
        page_lsn
    }

Leaf nodes contain sorted keys and associated values or record references.

Internal nodes contain separator keys and child page identifiers.

The exact persistent representation must be defined independently from in-memory C++ structures.

#### B+Tree Operations

**Search**

The search algorithm traverses internal nodes until it reaches the appropriate leaf.

**Insertion**

Insertion locates the target leaf, inserts the key/value pair, and triggers structural changes when capacity is exceeded.

**Splitting**

A leaf split redistributes entries between two pages and updates sibling relationships.

An internal split propagates separator information to the parent.

A root split creates a new root.

**Range Scan**

Range scans use the sorted key structure and leaf sibling links to traverse contiguous key ranges efficiently.

#### Future Extensions

- Deletion.
- Merge and redistribution.
- Concurrent B+Tree access.
- PageGuard integration.
- WAL-based structural recovery.
- MVCC-aware index entries.
- Unique and composite indexes.
- Prefix compression.
- Index-only scans.

### LSM Engine

The LSM Engine provides write-optimized storage based on sequential writes and background compaction.

It complements the B+Tree Engine by supporting workloads where write throughput and sequential storage behavior are important.

#### Architecture

    Write Request
         |
         v
       MemTable
         |
         v
     Immutable MemTable
         |
         v
     SSTable Flush
         |
         v
       SSTables
         |
         v
      Compaction

Read requests consult the in-memory and persistent structures according to the LSM lookup strategy.

#### Core Components

**MemTable**

An in-memory ordered data structure that stores recent writes.

A skiplist or another ordered structure may be used.

**Immutable MemTable**

A frozen MemTable that is no longer modified and is waiting to be flushed.

**SSTable**

An immutable sorted file containing ordered key/value entries.

SSTables may contain:

- Data blocks.
- Index blocks.
- Bloom filters.
- File metadata.
- Checksums.
- Sequence information.

**Bloom Filters**

Probabilistic structures that help avoid unnecessary SSTable reads.

**Compaction**

Background processing that merges sorted files, removes obsolete entries, and manages storage levels.

#### Read Path

    Read Request
         |
         v
      MemTable
         |
         v
   Immutable MemTables
         |
         v
       SSTables
         |
         v
    Bloom Filters
         |
         v
    Block Indexes
         |
         v
      Data Blocks

The engine must resolve multiple versions of a key according to sequence ordering and transaction visibility rules.

#### Write Path

    Write Request
         |
         v
       WAL Append
         |
         v
       MemTable
         |
         v
    MemTable Rotation
         |
         v
    Immutable MemTable
         |
         v
     SSTable Flush
         |
         v
      Manifest Update

The WAL provides recovery information for writes that have not yet been safely represented in persistent SSTables.

#### Future Extensions

- Multiple LSM levels.
- Size-tiered compaction.
- Leveled compaction.
- Tombstone management.
- Range deletion.
- SSTable compression.
- Block cache.
- Tiered storage.
- MVCC-aware compaction.
- Concurrent compaction scheduling.

### Transaction Layer

The Transaction Layer provides atomicity, consistency, isolation, and durability for database operations.

It coordinates modifications across storage engines and manages transaction visibility.

#### Architecture

    Client Operation
          |
          v
    Transaction Manager
          |
          +-------------------------+
          |                         |
          v                         v
      Lock Manager             MVCC Manager
          |                         |
          +------------+------------+
                       |
                       v
                Storage Engine
                       |
                       v
                   WAL Manager

#### Transaction Manager

Responsibilities:

- Begin transactions.
- Assign transaction identifiers.
- Track transaction state.
- Coordinate commit.
- Coordinate abort.
- Maintain transaction-local metadata.
- Integrate with WAL.
- Coordinate with storage engines.

#### Lock Manager

Provides concurrency control through shared and exclusive locks.

Responsibilities:

- Maintain lock tables.
- Manage lock compatibility.
- Track waiting transactions.
- Support lock upgrades.
- Detect deadlocks.
- Release locks at transaction completion.

Strict two-phase locking is a candidate concurrency-control strategy for transactions using pessimistic locking.

#### MVCC Manager

Provides multi-version concurrency control.

Responsibilities:

- Maintain transaction visibility metadata.
- Create new record versions.
- Track transaction snapshots.
- Resolve visible versions.
- Coordinate version cleanup.
- Integrate with transaction state.

Version metadata includes:

- `xmin`: Transaction that created a version.
- `xmax`: Transaction that invalidated a version.
- Previous-version reference.
- Version payload.

Transaction IDs and LSM sequence numbers are separate concepts and must not be conflated.

#### Snapshot Manager

Maintains transaction snapshots and visibility boundaries.

Responsibilities:

- Create snapshots.
- Track active transactions.
- Determine visible versions.
- Support read committed.
- Support repeatable read and snapshot isolation.
- Coordinate with version cleanup.

#### Isolation Levels

The transaction architecture should support:

- Read Uncommitted, if explicitly implemented.
- Read Committed.
- Repeatable Read.
- Snapshot Isolation.
- Serializable.

The exact guarantees of each isolation level must be defined by the concurrency-control implementation.

Snapshot isolation alone does not guarantee serializability.

#### Transaction Durability

A transaction must not be acknowledged as committed until its commit record is durable in the WAL.

The modified data pages may be flushed later.

This allows the database to use WAL-based recovery rather than requiring synchronous flushing of every modified page at commit.

#### Future Extensions

- Savepoints.
- Nested transaction semantics.
- Serializable Snapshot Isolation.
- Predicate locking.
- Transaction timeouts.
- Distributed transaction coordination.
- Group commit.
- Transaction monitoring.

### Replication Layer

The Replication Layer maintains copies of database state across multiple nodes.

Its primary responsibility is to keep replicas synchronized and support failover.

#### Architecture

    Client Write
         |
         v
       Leader
         |
         v
      WAL / Log
         |
         +-------------------+
         |                   |
         v                   v
      Follower A          Follower B
         |                   |
         v                   v
      Local Storage       Local Storage

#### Leader-Follower Model

Each replication group has one authoritative leader.

The leader coordinates writes and sends replication entries to followers.

Followers maintain replicated state and may serve reads according to the configured consistency guarantees.

#### WAL Shipping

The leader sends log entries to followers.

The replication layer tracks:

- Log positions.
- Replication progress.
- Acknowledgements.
- Commit state.
- Replica health.

The exact relationship between the database WAL and the replication log must be defined explicitly.

#### Replica Recovery

A replica that falls behind may catch up through retained log entries.

If the required log history is unavailable, it may need a snapshot transfer.

#### Failover

When a leader fails, a valid replica may become the new leader through the consensus protocol.

A former leader must not continue accepting authoritative writes after losing ownership.

#### Consistency

The replication layer should define:

- Write acknowledgement guarantees.
- Quorum requirements.
- Commit visibility.
- Follower read consistency.
- Replication lag behavior.
- Recovery guarantees.

#### Future Extensions

- Asynchronous replication.
- Synchronous replication.
- Read replicas.
- Multi-region replication.
- Snapshot transfer.
- Incremental replica recovery.
- Replication monitoring.
- Backup integration.

### Consensus Layer

The Consensus Layer coordinates authoritative decisions across distributed nodes.

It provides the safety guarantees needed for leader election and replicated log agreement.

MiniDB uses Raft as the initial consensus design.

#### Architecture

    Cluster
       |
       v
    Raft Group
       |
       +----------------------+
       |                      |
       v                      v
    Leader                 Followers
       |                      |
       v                      v
    Log Replication <--------+
       |
       v
    Commit Index
       |
       v
    State Machine Application

#### Raft Components

**Leader**

- Accepts proposals.
- Appends log entries.
- Replicates entries.
- Advances the commit index.
- Coordinates replication progress.

**Follower**

- Receives AppendEntries requests.
- Persists log entries.
- Responds to leader requests.
- Applies committed entries.

**Candidate**

- Initiates elections.
- Requests votes.
- Becomes leader after obtaining a majority.

#### Persistent State

Raft requires persistent state including:

- Current term.
- Voted-for candidate.
- Log entries.

Volatile state includes:

- Commit index.
- Last applied index.
- Leader replication progress.

#### Safety

Raft provides:

- Election safety.
- Leader append-only behavior.
- Log matching.
- Leader completeness.
- State-machine safety.

The database must preserve the distinction between an entry being replicated, committed, and applied.

#### Consensus and Database Transactions

Consensus establishes agreement on replicated log entries.

It does not automatically provide arbitrary multi-shard SQL transaction semantics.

Database transaction coordination remains the responsibility of the Transaction Layer and Distributed Storage Layer.

#### Future Extensions

- Dynamic membership.
- Snapshot installation.
- Log compaction.
- Membership reconfiguration.
- Multiple independent Raft groups.
- Joint consensus.
- Consensus monitoring.

### Distributed Storage Layer

The Distributed Storage Layer extends MiniDB across multiple storage nodes.

It manages data placement, ownership, query routing, and distributed transaction coordination.

#### Architecture

    Client
      |
      v
    Query Router
      |
      v
    Cluster Metadata
      |
      v
    Partition Manager
      |
      v
    Shard Manager
      |
      v
    Tablet Manager
      |
      v
    Replica Placement
      |
      +---------------------------+
      |              |            |
      v              v            v
    Node A         Node B       Node C
      |              |            |
      v              v            v
    Local Engine   Local Engine  Local Engine

#### Cluster Manager

Maintains cluster membership and node metadata.

#### Partition Manager

Defines logical data partitions and their boundaries.

#### Shard Manager

Maps logical data units to independently managed storage shards.

#### Placement Manager

Determines which nodes host primary and replica copies.

#### Consistent Hashing

Maps keys or logical ranges to placement positions while minimizing remapping when cluster membership changes.

Virtual nodes can improve distribution across heterogeneous cluster members.

Consistent hashing does not replace authoritative ownership metadata or consensus.

#### Tablet Manager

Manages the lifecycle of storage tablets.

Responsibilities:

- Tablet creation.
- Tablet assignment.
- Tablet splitting.
- Tablet merging.
- Tablet migration.
- Tablet recovery.

#### Query Router

Uses partition and tablet metadata to direct requests to the correct nodes.

It supports:

- Single-partition routing.
- Multi-partition routing.
- Leader-based writes.
- Configurable read routing.
- Scatter-gather execution.

#### Distributed Transaction Coordinator

Coordinates transactions spanning multiple shards.

Two-Phase Commit is the initial candidate for cross-shard atomic commit.

The coordinator must persist transaction decisions and support recovery of prepared participants.

#### Future Extensions

- Automatic rebalancing.
- Workload-aware placement.
- Distributed query optimization.
- Distributed joins.
- Multi-region deployment.
- Global transaction snapshots.
- Online repartitioning.
- Elastic cluster scaling.

## Read Path

The read path describes how a client request is resolved into a storage-engine operation and how the requested data is returned.

The exact path depends on the selected storage engine and deployment model.

### Local B+Tree Read Path

    Client
      |
      v
    Transaction Manager
      |
      v
    Snapshot / Visibility Check
      |
      v
    BPlusTree Search
      |
      v
    Buffer Pool Manager
      |
      +---------------------+
      |                     |
      v                     v
    Cache Hit             Cache Miss
      |                     |
      |                     v
      |                 Disk Manager
      |                     |
      +----------+----------+
                 |
                 v
          B+Tree Node
                 |
                 v
          Resolve Record
                 |
                 v
          MVCC Visibility
                 |
                 v
             Result

### Local B+Tree Read

1. Client submits a read request.
2. Transaction Manager establishes the transaction context.
3. Snapshot Manager determines the visibility boundary.
4. B+Tree searches the index.
5. Buffer Pool Manager retrieves the required page.
6. Disk Manager performs physical I/O on a cache miss.
7. The storage engine resolves the record.
8. MVCC determines whether the version is visible.
9. The result is returned.

### Local LSM Read Path

    Client
      |
      v
    Transaction Manager
      |
      v
    Snapshot Manager
      |
      v
    LSM Engine
      |
      +-----------------------+
      |                       |
      v                       v
    MemTable             Immutable MemTables
      |                       |
      +-----------+-----------+
                  |
                  v
               SSTables
                  |
                  v
             Bloom Filters
                  |
                  v
              Block Index
                  |
                  v
              Data Blocks
                  |
                  v
          Version Resolution
                  |
                  v
            MVCC Visibility
                  |
                  v
                Result

The engine must resolve multiple versions and tombstones before returning the visible value.

### Distributed Read Path

    Client
      |
      v
    Query Router
      |
      v
    Partition Pruning
      |
      v
    Tablet Resolution
      |
      v
    Replica Selection
      |
      v
    Storage Node
      |
      v
    Local Transaction Layer
      |
      v
    Local Storage Engine
      |
      v
    Result
      |
      v
    Client

For multi-partition reads, the distributed execution layer dispatches requests to multiple nodes and combines their results.

### Read Consistency

The read path must respect the requested consistency level.

Possible policies include:

- Leader reads.
- Follower reads with bounded staleness.
- Follower reads at a specified replication position.
- Linearizable reads using an appropriate consensus-backed read protocol.

A follower read must not be assumed linearizable merely because the follower belongs to a Raft group.

## Write Path

The write path describes how modifications move from the client through transaction processing and storage to durable persistence.

### Local Write Path

    Client
      |
      v
    Transaction Manager
      |
      v
    Lock / MVCC Validation
      |
      v
    Storage Engine
      |
      v
    WAL Manager
      |
      v
    WAL Append
      |
      v
    Buffer Pool / MemTable
      |
      v
    Dirty Page / MemTable State
      |
      v
    WAL Flush at Commit
      |
      v
    Commit Acknowledgement
      |
      v
    Background Page Flush / SSTable Flush

The precise ordering of WAL generation and in-memory modification must be enforced by the storage and transaction interfaces.

### B+Tree Write Path

1. Begin transaction.
2. Acquire required locks or establish MVCC write state.
3. Locate the target leaf page.
4. Generate the required WAL records.
5. Modify the buffer pool page.
6. Update the Page LSN.
7. Propagate structural changes when necessary.
8. Append the commit record.
9. Flush WAL through the commit LSN.
10. Acknowledge commit.
11. Flush dirty pages later.

B+Tree structural operations such as page splits and root changes must have explicit recovery semantics.

### LSM Write Path

1. Begin transaction.
2. Validate the write.
3. Generate WAL records.
4. Append records to WAL.
5. Update the MemTable.
6. Append the commit record.
7. Flush WAL through the commit LSN.
8. Acknowledge commit.
9. Rotate MemTables when required.
10. Flush immutable MemTables into SSTables.
11. Update the manifest safely.

The LSM engine must ensure that WAL reclamation does not occur before the corresponding data becomes safely represented in persistent storage.

### Distributed Write Path

    Client
      |
      v
    Query Router
      |
      v
    Partition / Tablet Mapping
      |
      v
    Leader Selection
      |
      v
    Transaction Manager
      |
      v
    Local Storage Engine
      |
      v
    WAL / Replication Log
      |
      v
    Raft Replication
      |
      v
    Quorum Commit
      |
      v
    State Machine Application
      |
      v
    Commit Acknowledgement

The exact relationship between database WAL persistence, Raft log persistence, and state-machine application must be specified before implementing distributed writes.

The architecture must not assume that Raft commitment alone automatically makes arbitrary database page modifications durable.

### Distributed Transaction Write Path

For a transaction involving multiple shards:

1. The coordinator assigns a global transaction ID.
2. The query router identifies participating shards.
3. Each participant executes its local modifications.
4. The coordinator initiates the prepare phase.
5. Participants persist prepared state.
6. The coordinator persists the final decision.
7. The decision is propagated to participants.
8. Participants commit or abort locally.
9. The client receives the final transaction outcome.

The transaction coordinator must recover incomplete distributed transactions after failure.

## Recovery Flow

Recovery restores database state after an unexpected process, machine, or storage failure.

The architecture combines local WAL-based recovery with replica and cluster-level reconciliation.

### Local Recovery

    Database Startup
          |
          v
    Open Database Files
          |
          v
    Open WAL
          |
          v
    Locate Checkpoint
          |
          v
       Analysis
          |
          v
         REDO
          |
          v
         UNDO
          |
          v
    Persist Recovery State
          |
          v
    Database Ready

### Recovery Responsibilities

**WAL Manager**

Provides validated log records and durable LSN information.

**Recovery Manager**

Coordinates analysis, REDO, UNDO, and recovery completion.

**Buffer Pool Manager**

Provides controlled page access during recovery.

**Transaction Manager**

Reconstructs transaction state and handles incomplete transactions.

**Checkpoint Manager**

Provides the recovery boundary and checkpoint metadata.

**Storage Engine**

Defines how its data structures and operations are replayed or undone.

### Distributed Recovery

    Node Restart
        |
        v
    Local WAL Recovery
        |
        v
    Load Tablet Metadata
        |
        v
    Reconcile Replication Position
        |
        v
    WAL Catch-Up or Snapshot Transfer
        |
        v
    Validate Replica State
        |
        v
    Rejoin Cluster

A locally recovered node is not automatically eligible to become leader or serve authoritative reads.

It must also satisfy replication and ownership requirements.

### Recovery Invariants

- WAL must be durable before dependent dirty pages are persisted.
- Committed transactions must survive crashes.
- Incomplete transactions must be rolled back.
- REDO must be idempotent.
- UNDO must be restartable.
- Checkpoints must be crash-safe.
- WAL must not be reclaimed while required for recovery.
- Replicas must not serve authoritative data before state reconciliation.
- Distributed transaction decisions must be recoverable.
- Stale ownership information must not authorize conflicting writes.

### Recovery and Storage Engines

Each storage engine must define its own recovery operations.

For B+Tree:

- Page modifications.
- Leaf and internal splits.
- Root changes.
- Page allocation and deallocation.
- Index metadata updates.

For LSM:

- MemTable recovery.
- WAL replay.
- SSTable flush completion.
- Manifest updates.
- Compaction recovery.
- Tombstone and version cleanup.

The Recovery Manager coordinates recovery, but storage-engine-specific logic remains owned by the corresponding engine.

## Replication Flow

Replication maintains consistent copies of data across multiple nodes.

The architecture uses leader-follower replication coordinated by the consensus layer.

### Replication Architecture

    Client
      |
      v
    Leader
      |
      v
    Replication Log
      |
      +----------------------+
      |                      |
      v                      v
    Follower A             Follower B
      |                      |
      v                      v
    Persistent Log          Persistent Log
      |                      |
      v                      v
    Apply Committed         Apply Committed
    Entries                 Entries

### Write Replication

1. Client sends a write to the leader.
2. The leader validates the request.
3. The leader appends a log entry.
4. The entry is persisted according to the protocol.
5. The leader sends replication requests to followers.
6. Followers validate and persist the entries.
7. The leader determines when the entry is committed.
8. Committed entries are applied to the state machine.
9. The client receives acknowledgement according to the configured durability and consistency guarantees.

### Replication State

The replication layer tracks:

- Current term.
- Leader identity.
- Log position.
- Commit index.
- Applied index.
- Follower replication progress.
- Replica health.
- Snapshot position.

Raft-specific state and database transaction state must remain distinct.

### Replica Catch-Up

A follower that falls behind may receive missing log entries.

If the required log history has been compacted or removed, the leader may transfer a snapshot.

After snapshot installation, the follower continues with subsequent log entries.

### Failover

When the leader fails:

1. Followers detect missing leader communication.
2. Election timers trigger candidacy.
3. A candidate requests votes.
4. A candidate receiving a majority becomes leader.
5. The new leader establishes its replication state.
6. The cluster updates ownership metadata.
7. Clients refresh stale routing information.

The consensus protocol determines which replica is eligible to become leader.

### Replication and Local Recovery

Replication and local recovery have different responsibilities:

- Local recovery restores a node's database state from its WAL.
- Replication recovery synchronizes that node with the authoritative replica group.
- Consensus establishes authoritative leadership and committed log history.

These operations must be coordinated during node restart and failover.

### Replication and Database WAL

The relationship between the database WAL and Raft log is an important architectural decision.

Possible designs include:

**Shared log**

A single durable log represents both database modifications and replicated state-machine commands.

Advantages:

- Fewer duplicated logging mechanisms.
- A direct relationship between replicated entries and database operations.

Challenges:

- Database WAL semantics and Raft log semantics may differ.
- Log compaction and checkpointing must satisfy both systems.
- Transaction and consensus metadata must coexist safely.

**Separate logs**

The Raft log contains replicated commands, while the database WAL contains local storage-engine modifications.

Advantages:

- Independent lifecycle and recovery policies.
- Clear separation between consensus and storage-engine logging.

Challenges:

- Requires coordination between replicated command application and local database persistence.
- Must prevent inconsistencies between committed Raft entries and database state.

The selected approach must define the durability and replay relationship between the two logs.

## Future Enhancements

MiniDB is designed to evolve beyond its initial B+Tree-based storage implementation.

Future development should focus on expanding storage capabilities, improving concurrency, strengthening durability, and introducing distributed database functionality.

### Storage Engine Enhancements

- B+Tree deletion, merge, and redistribution.
- Concurrent B+Tree operations.
- PageGuard and latch management.
- LSM MemTable and SSTable implementation.
- Multi-level compaction.
- Compression.
- Tiered storage.
- Hybrid B+Tree and LSM architecture.

### Indexing Enhancements

- Secondary indexes.
- Composite indexes.
- Unique indexes.
- Covering indexes.
- Full-text indexing.
- Vector indexing.
- Approximate nearest-neighbor search.
- Hybrid vector and keyword search.

### Transaction Enhancements

- Full MVCC integration.
- Serializable isolation.
- Predicate locking.
- Distributed snapshots.
- Savepoints.
- Transaction monitoring.
- Group commit.
- Distributed transaction recovery.

### Query Processing Enhancements

- SQL parser.
- Logical query planner.
- Cost-based optimizer.
- Physical execution operators.
- Join algorithms.
- Parallel query execution.
- Predicate pushdown.
- Index selection.
- Query plan caching.
- Distributed query optimization.

### Durability and Recovery Enhancements

- ARIES-inspired recovery.
- Compensation Log Records.
- Fuzzy checkpoints.
- Full-page images.
- Torn-page protection.
- WAL segmentation.
- Point-in-time recovery.
- Backup and restore.
- Recovery diagnostics.

### Concurrency Enhancements

- Concurrent buffer pool.
- PageGuard.
- Latch coupling.
- Concurrent B+Tree access.
- Background flushing.
- Asynchronous I/O.
- Lock-free or low-contention data structures where appropriate.
- Thread scheduling and workload isolation.

### Replication and Consensus Enhancements

- Snapshot transfer.
- Dynamic membership.
- Multi-Raft groups.
- Replica placement policies.
- Automatic failover.
- Read replicas.
- Multi-region replication.
- Replication monitoring.

### Distributed Storage Enhancements

- Automatic sharding.
- Consistent hashing.
- Tablet splitting and merging.
- Online repartitioning.
- Workload-aware placement.
- Automatic rebalancing.
- Distributed transactions.
- Distributed joins.
- Elastic cluster scaling.

### Observability and Operations

- Structured logging.
- Metrics and dashboards.
- Distributed tracing.
- Query execution profiling.
- WAL and checkpoint monitoring.
- Buffer pool statistics.
- Lock contention metrics.
- Compaction metrics.
- Replication lag monitoring.
- Cluster health reporting.
- Recovery progress reporting.

### Security and Reliability

- Authentication.
- Authorization.
- Encryption at rest.
- Encryption in transit.
- Audit logging.
- Backup validation.
- Data corruption detection.
- Fault injection testing.
- Chaos testing.
- Resource isolation.

### Architectural Evolution

The implementation should progress from a reliable local storage engine toward a complete distributed database.

The intended progression is:

    Storage Foundations
          |
          v
    B+Tree Storage Engine
          |
          v
    WAL and Crash Recovery
          |
          v
    Transaction Processing
          |
          v
    MVCC and Concurrency
          |
          v
    LSM Storage Engine
          |
          v
    Query Processing
          |
          v
    Replication
          |
          v
    Raft Consensus
          |
          v
    Distributed Storage
          |
          v
    Distributed Transactions
          |
          v
    Distributed Query Execution

### Final Architectural Principle

MiniDB is designed as a layered database system in which storage engines, common infrastructure, transaction processing, recovery, replication, consensus, and distributed storage have clearly defined responsibilities.

The local storage engine provides reliable data structures and persistence. The transaction layer provides atomicity and isolation. WAL and recovery provide durability. Replication maintains copies of database state. Consensus coordinates authoritative decisions. Distributed storage manages data placement and request routing.

The architecture must preserve these boundaries as the system evolves.

Every new capability should integrate through explicit interfaces, preserve existing correctness invariants, and remain independently testable.

The objective is not merely to combine multiple database technologies, but to build a coherent database architecture in which each subsystem contributes to a well-defined set of correctness, performance, and scalability guarantees.