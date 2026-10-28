# NextFS — design (v0.1, draft)

NextFS is the NextNT native file system. It replaces NTFS: there is no `$MFT`, no
journal replay of file records, no directory indexes (`$I30`) and no change journal
(`$UsnJrnl`). The design centre is the **PIFT**, the *Pre-Indexed File Table*.

This document is the contract between four components that must agree byte for byte:

| Component | Path | Role |
|---|---|---|
| `nextfslib` | `sdk/lib/fslib/nextfslib/` | on-disk format, PIFT codec, in-RAM index, allocator helpers (portable C, no kernel deps) |
| `nextfs.sys` | `drivers/filesystems/nextfs/` | the FSD (IRP-based, modelled on `fastfat`) |
| `nextformat` | `sdk/lib/fslib/nextfslib/tools/` | mkfs |
| boot chain | `boot/nextldr/nextldr/lib/fs/nextfs.c` (later phase) | must read a NextFS volume to boot |

Status: **phase 0-1**. `nextfslib` + `nextformat` are implemented and self-tested;
`nextfs.sys` is a mount-capable skeleton (volume open + PIFT load, I/O paths still
`STATUS_NOT_IMPLEMENTED`, marked `TODO(phase2)`).

---

## 1. Physical layout

Everything is a multiple of the cluster size (default 4096 bytes).

```
cluster 0            boot stub (OEM "NEXTFS  "), reserved, never written by the FS
cluster 1            superblock, primary copy
cluster 2 .. N       metadata area: copy-on-write B+tree nodes, journal, PIFT area
...
last cluster         superblock, backup copy
```

The boot stub at cluster 0 keeps the classic 512-byte shape (jump + OEM + BPB-ish
fields + `0xAA55`) so the existing MBR/VBR code paths and `fs_rec` have something to
recognise. The real metadata is *not* described by the BPB: it is described by the
superblock, which is authoritative.

### 1.1 Superblock (`NFSB`)

1024 bytes, written twice (cluster 1 and the last cluster of the volume). Mount uses
the copy with the higher `generation` whose CRC is valid; if both are valid and equal,
the primary is used.

| Field | Type | Meaning |
|---|---|---|
| `magic` | `char[4]` | `"NFSB"` |
| `version` | `u16` | format version, currently 1 |
| `header_size` | `u16` | 1024 |
| `cluster_size` | `u32` | bytes per cluster, power of two, >= 4096 |
| `total_clusters` | `u64` | volume size in clusters |
| `volume_uuid` | `u8[16]` | stable identity of the volume |
| `generation` | `u64` | bumped on every committed transaction |
| `journal_first` | `u64` | first cluster of the journal ring |
| `journal_clusters` | `u32` | journal length in clusters |
| `journal_seq_first` | `u64` | oldest transaction still recoverable |
| `journal_seq_next` | `u64` | next transaction sequence number |
| `pift_first` | `u64` | first cluster of the PIFT-on-disk area |
| `pift_clusters` | `u32` | length of the PIFT-on-disk area |
| `pift_seq_next` | `u64` | next PIFT record sequence number |
| `pift_root` | `{ u64 block, u64 generation }` | checkpoint root of the PIFT tree |
| `tree_root` | `{ u64 block, u64 generation }` | namespace tree root (directory links) |
| `refcount_root` | `{ u64 block, u64 generation }` | copy-on-write block refcount tree |
| `bitmap_root` | `{ u64 block, u64 generation }` | allocation bitmap (copy-on-write chain) |
| `free_clusters` | `u64` | advisory free count (the bitmap is authoritative) |
| `flags` | `u32` | bit 0 dirty, bit 1 needs-check, bit 2 read-only |
| `label` | `char[64]` | UTF-8 volume label, NUL padded |
| `crc32c` | `u32` | CRC32C over bytes 0..1019 |

A `{block, generation}` pair is read as: "block *n* is the root **only** if the node
stored there carries generation *g*". Stale blocks (lower generation) are ignored.
That single rule is what makes copy-on-write cheap and snapshots free.

### 1.2 Copy-on-write B+tree node (4 KiB)

| Field | Type | Meaning |
|---|---|---|
| `magic` | `char[4]` | `"NFBT"` |
| `level` | `u16` | 0 = leaf |
| `entries` | `u16` | number of (key, value) pairs |
| `generation` | `u64` | written with the node, compared on read |
| `block` | `u64` | self address (detects a block copied to the wrong place) |
| `keys` / `values` | arrays | leaf: `u64 key` + opaque value; internal: `u64 key` + `u64 child` |
| `crc32c` | `u32` | over the node |

Keys are `u64`. Three trees are used:

* **namespace tree** — key = `hash(parent_fileid, casefold(name))` + name bytes;
  used for directory enumeration ordering and for hard-link records.
* **PIFT checkpoint tree** — key = `fileid`; value = a serialised PIFT record
  (section 2). This is the compacted form of the PIFT-on-disk area.
* **refcount tree** — key = `block`; value = `refcount`. Required because a
  copy-on-write block may be shared by a snapshot and the live tree.

### 1.3 Allocation bitmap

The bitmap is one bit per cluster, stored as a chain of copy-on-write blocks (a
buddy-style two-level index: level-0 block covers 4 KiB * 8 = 32768 clusters = 128 MiB
at 4 KiB clusters). The allocator reserves the tail of the volume as a "metadata
zone" so that metadata does not fragment into the data area.

### 1.4 Journal (WAL)

The journal is a ring of clusters made of 4 KiB frames:

| Field | Type | Meaning |
|---|---|---|
| `magic` | `char[4]` | `"NFJN"` |
| `seq` | `u64` | transaction sequence number |
| `flags` | `u32` | bit 0 committed, bit 1 checkpoint |
| `payload_len` | `u32` | bytes of record payload |
| `crc32c` | `u32` | over header (CRC zeroed) + payload |
| `payload` | bytes | serialised PIFT records / extent updates |

Rules:

1. A transaction is **committed** only when its journal frame with `flags & COMMITTED`
   is fully on disk (single sector-ish write, ordered by explicit flush).
2. Only after the commit frame is durable may the PIFT-on-disk area be appended and
   the superblock generation bumped (checkpoint).
3. Replay at mount walks from `journal_seq_first` to `journal_seq_next`, skips
   non-committed frames and torn frames (CRC mismatch ⇒ stop), and re-applies payloads
   idempotently (PIFT records carry their own `seq`, highest `seq` per fileid wins).

### 1.5 PIFT-on-disk area

Append-only log of serialised PIFT records, wrapping inside `pift_first ..
pift_first + pift_clusters`. It is **write-through**: every committed change appends
its record(s) here, so the on-disk table always trails the RAM table by at most one
transaction. It is **never read during runtime**: the only readers are

* mount (to rebuild the RAM table), and
* `nextchk` (recovery) when the RAM table cannot be rebuilt or the volume is dirty.

When the area reaches a high-water mark, it is **compacted**: the live records are
copied into a fresh PIFT checkpoint tree, `pift_root` is swapped with a single
superblock write, and the area is reset. Compaction is itself copy-on-write, so a
crash mid-compaction leaves the old root valid.

---

## 2. The PIFT

A PIFT record is the entire identity of a file. There is no separate "directory
entry" object, no `$STANDARD_INFORMATION`/`$FILE_NAME` attribute soup: one record
holds everything, and the RAM table is the index over it.

```
struct nfs_pift_record {
    u64  fileid;            /* stable, never reused within a volume      */
    u64  generation;        /* bumped on delete+recreate (ABA guard)     */
    u64  parent;            /* fileid of the parent directory            */
    u64  seq;               /* PIFT-on-disk sequence, highest wins       */
    u32  flags;             /* DIR | DELETED | COMPRESSED | SPARSE       */
    u32  attributes;        /* Win32 FILE_ATTRIBUTE_* bitmask            */
    u64  size;              /* logical size in bytes                     */
    u64  alloc_size;        /* bytes actually mapped                    */
    u64  ctime, mtime, atime, btime;   /* 100ns since 1601, NT semantics  */
    u32  name_len;          /* UTF-16 code units, WITHOUT terminator     */
    u16  name[255];         /* the long name; 8.3 names are not stored   */
    u32  extent_count;
    struct nfs_extent {     /* logical -> physical mapping              */
        u64  vcn;           /* first virtual cluster                    */
        u64  lcn;           /* first physical cluster, or NFS_HOLE      */
        u64  length;        /* clusters; 0 terminates the list          */
    } extents[];
};
```

### 2.1 In-RAM layout

The RAM table is authoritative and holds, per file:

* an **open-addressing hash** `fileid → record slot` (power-of-two capacity, linear
  probing, tombstones, 70% load factor);
* a **child index** `parent_fileid → sorted list of child slots` (kept sorted by
  case-folded name, so directory enumeration is a memcpy and `NtQueryDirectoryFile`
  with `RestartScan` is O(1) to resume);
* a **path cache** (dentry-style, validated by `generation`).

Lookups by name are resolved by walking the child index; there is no on-disk
directory index to read, which is exactly why **indexed search is pointless on
NextFS** and why nothing in this design implements one. `$I30`, `$UsnJrnl` and the
NTFS change journal have no equivalent.

### 2.2 Write-through discipline

For a change that touches files A and B:

1. Take the volume intent lock, allocate a `seq` from `pift_seq_next`.
2. Serialise the new PIFT records (A, B) into one transaction payload.
3. Append the frame to the journal ring, `flags = 0`, flush.
4. Append the same payload to the PIFT-on-disk area, flush.
5. Mark the journal frame `COMMITTED` (one 4 KiB write), flush. **This is the commit
   point.**
6. Apply to RAM: update slots, child index, path cache. Release the lock.
7. Asynchronously (checkpointing thread): copy-on-write the touched metadata blocks,
   swap `pift_root`/`tree_root`, bump `generation` in both superblock copies, then
   retire journal frames below `journal_seq_first`.

Crash analysis:

| Crash point | Result on next mount |
|---|---|
| before 5 | frame not committed ⇒ discarded; RAM table never saw it ⇒ no visible change |
| after 5, before 6 | replay re-applies the payload; RAM table is rebuilt from disk anyway ⇒ change applied |
| after 6, before 7 | PIFT-on-disk is ahead of the checkpoint tree; mount loads the area first, then the tree, and takes the highest `seq` per fileid |
| during compaction | old `pift_root` still referenced by the superblock ⇒ old snapshot wins |

Because the PIFT-on-disk area is append-only and versioned by `seq`, there is no
"is the table consistent?" question at boot: replay plus highest-`seq`-wins is enough.

### 2.3 Memory cost and limits

The RAM table is `sizeof(record) + name + extents` per file, roughly 200-400 bytes for
typical files, plus 8 bytes per hash slot. A volume with 1 million files costs on the
order of 300-500 MB of kernel pool, which is why the design keeps records compact,
stores names as UTF-16 without termination, and puts extents in a tail array (most
files have one or two). A small-file threshold stores data inline in the record
(`flags & INLINE_DATA`), mirroring the "resident data" idea of NTFS but inside the
PIFT record.

---

## 3. Copy-on-write

* No metadata block, bitmap block or data page is ever rewritten in place while a
  snapshot may reference it.
* A write to a file allocates new clusters, writes the payload, then atomically
  publishes the new extent list through the PIFT record + journal commit. Old
  clusters drop one refcount; at zero they return to the bitmap.
* A snapshot is `{pift_root, tree_root, refcount_root, bitmap_root, generation}`,
  i.e. 5 numbers. Creating one is a superblock-adjacent metadata write; deleting one
  runs a garbage collector over the refcount tree.
* Full-file atomic replace (`ReplaceFile`, `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`)
  is a single PIFT record commit — no rename-with-delete-window.

---

## 4. What replaces what

| NTFS today | NextFS |
|---|---|
| `$MFT` + `$MFTMirr` | PIFT in RAM; PIFT-on-disk area + PIFT checkpoint tree |
| MFT record `FILE` with attributes | one flat `nfs_pift_record` |
| `$DATA` run lists, resident/non-resident | `extents[]`, plus inline data for small files |
| `$INDEX_ROOT`/`$INDEX_ALLOCATION` (`$I30`) | child index in RAM (sorted), namespace tree on disk |
| `$Bitmap` | copy-on-write bitmap chain |
| `$LogFile` | journal ring, commit-frame protocol (section 2.2) |
| `$UsnJrnl`, indexed search, `$Secure`, `$UpCase` | nothing: name comparison uses a casefold table compiled in (`nextfslib/casefold`), security descriptors live in the PIFT record tail (phase 3) |
| `fs_rec` OEM "NTFS    " | OEM "NEXTFS  " + superblock magic |

---

## 5. Open questions (to settle before phase 3)

1. **Security descriptors**: store inline in the record (simple, fat) or in a
   deduplicated blob store (smaller, slower)? Leaning to a deduplicated store keyed by
   CRC, like `$Secure` but without the index.
2. **Compression/encryption**: NTFS does it in the FSD above the `$DATA` attribute.
   NextFS can do it per-extent with a flag, but the cache manager path must not see
   compressed bytes. Deferred.
3. **Boot path**: `nextldr` must map the boot volume. Simplest correct approach is a
   read-only NextFS reader in `boot/nextldr/nextldr/lib/fs/nextfs.c` that uses only the
   superblock + PIFT checkpoint tree (never the journal), so boot cannot be broken by a
   dirty volume.
4. **Journal sizing**: fixed 8 MiB or scale with volume? Sizing rule goes in the
   superblock at format time.

---

## 6. Implementation status (v0.1, branch `nextfs`)

Done and tested in this phase:

| Piece | Files | Verified by |
|---|---|---|
| on-disk format, superblock, bitmap | `nextfslib/nextfs_super.c` | format + mount + remount in the self-test |
| PIFT record codec + RAM index | `nextfslib/nextfs_pift.c` | create/lookup/readdir/unlink checks |
| journal, commit frames, replay | `nextfslib/nextfs_journal.c` | "crash after commit" and "torn commit frame" cases |
| transactions, checkpoint chain | `nextfslib/nextfs_volume.c` | remount after clean unmount, free-space accounting |
| mkfs / inspector | `nextfslib/tools/nextformat.c`, `nextfsinfo.c` | 16 MiB image formatted and listed |
| driver skeleton | `drivers/filesystems/nextfs/nextfs.c` | not compiled yet (needs the RosBE build) |

`nextfslib/tests/nextfs_selftest.c` runs 40 checks: format, mount, create/write/
read, mkdir, nested file, append, readdir, unmount+remount (PIFT rebuilt from
disk only), crash after the commit point (change must survive), crash before the
commit frame (change must be discarded), delete, free-space return.

Deliberate v0.1 limitations, all scheduled for phase 2:

1. **No snapshots / refcount tree.** Copy-on-write is used for metadata blocks and
   for whole-file rewrites; there are no snapshots yet, so nothing needs block
   refcounts. `NfsWrite` in the middle of a file returns `NFS_EUNSUPPORTED`
   (needs extent splitting), and an unaligned append patches the tail cluster in
   place instead of shadowing it.
2. **Checkpoint chain is a leaf chain**, not a multi-level B+tree: fine for the
   tens of thousands of records a test volume holds, phase 2 for a real volume.
3. **Security descriptors, compression, sparse files**: not implemented.
4. **No kernel I/O paths**: `nextfs.sys` mounts and loads the PIFT, everything
   else is `STATUS_NOT_IMPLEMENTED`. `nextfslib` is therefore not yet wired into
   `sdk/lib/fslib/CMakeLists.txt` and the driver is not in
   `drivers/filesystems/CMakeLists.txt`: NTFS stays the boot file system until
   phase 2 lands.
5. **Child lookup is a linear scan** over the RAM records; the sorted child index
   described in 2.1 is phase 2 (it is an in-RAM structure, so the on-disk format
   does not change when it lands).
