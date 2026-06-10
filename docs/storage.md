
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

The Disk Manager provides a low-level abstraction over the database file.
The bottom of the storage stack, everything else(page header, page manager, buffer pool, WAL)
sits on top of sits on top of this Disk manager (“move bytes between memory and data/*.db.”)

Responsibilities:

* Open database file (e.g. data/minidb.db (matches .gitignore’s data/*.db))
* PAGE_SIZE = 4096; Offset = page_id * 4096; no checksums, no free-list, no buffer pool
* Read page from disk
* Write page to disk
* Allocate space for new pages
* Flush writes

The Disk Manager does not understand page contents.

It only moves bytes between memory and disk.

---

## Page Manager

The Page Manager manages logical pages.

Responsibilities:

* Allocate page IDs
* Create pages
* Track metadata
* Interface between Buffer Pool and Disk Manager

Every page is identified by a unique PageId.

---

## Page Layout

Each page is fixed size.

Initial implementation:

```text
PAGE_SIZE = 4096 bytes
```

Layout:

```text
+------------------------+
| Page Header            |
+------------------------+
| User Data              |
|                        |
|                        |
+------------------------+
```

---

## Page Header

```cpp
struct PageHeader {
    uint32_t page_id;
    uint32_t page_lsn;
    uint32_t free_space_offset;
    uint32_t checksum;
};
```

Purpose:

* Page identification
* Recovery support
* Free space tracking
* Corruption detection

---

## Page Structure

```cpp
class Page {
public:
    PageId page_id;

    bool is_dirty;
    int pin_count;

    char data[PAGE_SIZE];
};
```

---

## Page IDs

Page IDs are monotonically increasing.

```text
0
1
2
3
...
```

Page 0 is reserved for metadata.

---

## Read Path

```text
PageManager
    |
    v
DiskManager
    |
    v
Read Page N
```

---

## Write Path

```text
PageManager
    |
    v
DiskManager
    |
    v
Write Page N
```

---

## Future Enhancements

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
