
# Storage Layer

## Overview

The Storage Layer is the foundation of MiniDB.

Its responsibility is to provide persistent page-based storage abstraction over physical files on disk.

All higher-level components such as the Buffer Pool, B+Tree, WAL, Recovery, MVCC, and LSM Engine operate on pages managed by the Storage Layer.

---

## Goals

* Persistent storage
* Fixed-size pages
* Efficient page allocation
* Page-level reads and writes
* Stable page identifiers
* Simple and predictable API

---

## Architecture

```text
B+Tree / WAL / BufferPool
            |
            v
      Page Manager
            |
            v
      Disk Manager
            |
            v
        data.db
```

---

## Disk Manager

The Disk Manager provides a low-level abstraction over the database file. It sits at the bottom of the storage stack. Higher-level components—including PageManager, BufferPool, and WAL—use it for file I/O.

Its primary responsibility is to move fixed-size pages between memory and the database file (data/minidb.db).

Responsibilities:

* Open or create the database file (e.g., data/minidb.db, matching .gitignore's data/*.db rule).
* Define PAGE_SIZE = 4096 bytes.
* Calculate page offsets: offset = page_id * PAGE_SIZE.
* Read a page from the file.
* Write a page to the file.
* Flush buffered writes and report I/O errors.

Non-Responsibilities:

* Allocate or deallocate database pages.
* Interpret page headers, page types, or metadata.
* Maintain a free-page list or allocation bitmap.
* Manage the buffer pool or page eviction.
* Implement checksums, WAL, or crash recovery.

The Disk Manager does not understand page contents, it treats page contents as opaque bytes. It does not know whether a page contains database metadata, a B+Tree node, or LSM data.

Page allocation and page-ID management belong to PageManager. BufferPool will later manage cached pages in memory, while WAL and recovery will handle durability and crash consistency.

Disk Manager only moves bytes between memory and disk.

Current limitations: 

* flush() flushes the C++ stream buffer but does not guarantee that data has reached durable storage. Crash durability will be addressed in a later milestone.

---

## Page Manager

The Page Manager manages logical pages.

Responsibilities:

* Allocate page IDs
* Create pages
* Track metadata
* Interface between Buffer Pool and Disk Manager

Every page is identified by a unique PageId.


### Page Format

MiniDB uses fixed-size pages of 4096 bytes. The Disk Manager treats each page as opaque bytes; PageManager and higher-level storage components define and interpret page contents.

### Page IDs

- Page IDs are unsigned 64-bit integers (`uint64_t`).
- Page 0 is reserved for database metadata.
- Data pages begin at page ID 1.
- The next page ID is stored in page 0's metadata.

### Database Metadata Page (Page 0)

The first 16 bytes contain the metadata header.

| Field | Offset | Size |
|---|---:|---:|
| Magic number | 0 | 4 bytes |
| Format version | 4 | 2 bytes |
| Metadata header size | 6 | 2 bytes |
| Next page ID | 8 | 8 bytes |

The remaining 4080 bytes are reserved for future metadata.

The magic number identifies a MiniDB database file. The format version allows future versions to detect incompatible on-disk layouts. The next page ID identifies the next data-page ID to allocate.

### Data Page Layout

Every data page has a 16-byte common header followed by a 4080-byte payload.

| Field | Offset | Size |
|---|---:|---:|
| Page ID | 0 | 8 bytes |
| Page type | 8 | 2 bytes |
| Format version | 10 | 2 bytes |
| Flags | 12 | 4 bytes |
| Payload | 16 | 4080 bytes |

### Page Types

Page types use stable numeric identifiers.

| Type | Value | Purpose |
|---|---:|---|
| `META` | 1 | Database metadata |
| `BTREE_LEAF` | 2 | B+Tree leaf node |
| `BTREE_INTERNAL` | 3 | B+Tree internal node |
| `LSM_DATA` | 4 | LSM data page |
| `FREE` | 5 | Page available for future reuse |

Page 0 uses the metadata layout rather than the common data-page header.

### Encoding and Initialization Rules

- Multi-byte fields use little-endian byte order.
- Page headers are encoded and decoded explicitly; raw C++ structs are not written directly to disk.
- Newly allocated data pages begin zero-filled, after which the common page header is initialized.
- Invalid page IDs and unsupported format versions must be rejected.
- Page allocation metadata is persisted, but allocation is not yet crash-atomic. WAL and recovery will address crash consistency in a later milestone.

### In-Memory Page State

Runtime state associated with cached pages is separate from the on-disk page format.

The future BufferPool may track:

- `is_dirty`: whether a cached page has been modified.
- `pin_count`: the number of active users holding a page pinned.
- Cache and eviction metadata.

These fields are maintained in memory and are not part of the persistent page header.

### Read Path

```text
PageManager
    |
    v
DiskManager
    |
    v
Read page N from the database file
```

### Write Path

```text
PageManager
    |
    v
DiskManager
    |
    v
Write page N to the database file
```

### Deferred Page Header Fields

The initial page format does not include the following fields. They may be introduced when the corresponding features are designed:

- `page_lsn`: supports WAL-based recovery and page-version tracking.
- `free_space_offset`: tracks free space in page layouts that need it, such as variable-length records.
- `checksum`: helps detect page corruption.


### Future Enhancements

The initial implementation uses append-only allocation. Free-page reuse, checksums, allocation bitmaps, and crash-safe allocation are deferred until later milestones.
* Free page list
* Checksums
* Compression
* Multiple database files
* Direct I/O
* Asynchronous I/O

---

## Design Decisions

### Why fixed-size pages?

Fixed-size pages simplify:

* Buffer Pool
* WAL
* Recovery
* B+Tree nodes

### Why 4 KB pages?

4 KB matches:

* OS page size
* PostgreSQL page model
* Common storage engine design

---

## Open Questions

### Variable Page Size Support

Current Decision:
- No

Rationale:
- Fixed-size pages significantly simplify Buffer Pool, WAL, Recovery, and B+Tree implementations.
- Most production databases use a fixed page size.
- MiniDB will initially use 4 KB pages.

Future Consideration:
- Support configurable page sizes (4 KB, 8 KB, 16 KB) at database creation time.

---

### Compression

Current Decision:
- No

Rationale:
- Compression complicates page management, free-space tracking, and recovery.
- Not required for understanding storage engine fundamentals.

Future Consideration:
- Page-level compression.
- SSTable block compression for the LSM engine.
- Snappy/LZ4-style compression similar to RocksDB.

---

### Page Checksums

Current Decision:
- Not in Phase 1

Rationale:
- Checksums are useful for corruption detection but are not necessary for initial storage engine development.
- They introduce additional write-path overhead and recovery logic.

Future Consideration:
- Add CRC32 checksum to page header.
- Verify checksum during page reads.
- Similar to PostgreSQL page checksums.

---

### Separate Metadata File

Current Decision:
- No

Rationale:
- Metadata will initially be stored in reserved pages inside the main database file.
- Simpler implementation and recovery model.

Future Consideration:
- Dedicated metadata file.
- Catalog pages.
- Free page map.
- Space management structures.
