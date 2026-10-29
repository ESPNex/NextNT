# NextFS — design (v0.9, branch `nextfs`)

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

Status: **phase 9**. `nextfslib` implements the format as described here and is
self-tested (310 checks); `nextfs.sys` is still a mount-capable skeleton (volume
open + PIFT load, I/O paths `STATUS_NOT_IMPLEMENTED`).

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
| `version` | `u16` | format version, currently 2 |
| `header_size` | `u16` | 1024 |
| `cluster_size` | `u32` | bytes per cluster, power of two, >= 4096 |
| `total_clusters` | `u64` | volume size in clusters |
| `volume_uuid` | `u8[16]` | stable identity of the volume |
| `generation` | `u64` | bumped on every committed transaction |
| `journal_first` | `u64` | first cluster of the journal ring |
| `journal_clusters` | `u32` | journal length in clusters |
| `journal_seq_first` | `u64` | oldest transaction still recoverable |
| `journal_seq_next` | `u64` | next transaction sequence number |
| `pift_area_first` | `u64` | first cluster of the whole PIFT-on-disk area |
| `pift_area_clusters` | `u32` | size of the whole PIFT-on-disk area (two halves) |
| `pift_first` | `u64` | first cluster of the half the live generation writes |
| `pift_clusters` | `u32` | length of that half |
| `pift_head` | `u64` | bytes appended in that half |
| `pift_seq_next` | `u64` | next PIFT record sequence number |
| `checkpoint_root` | `{ u64 block, u64 generation }` | live PIFT checkpoint tree |
| `checkpoint_root_prev` | `{ u64 block, u64 generation }` | previous generation, the fallback |
| ~~`tree_root`~~, ~~`refcount_root`~~, ~~`bitmap_root`~~ | | planned (section 3, open item 1) |
| `free_clusters` | `u64` | advisory free count (the bitmap is authoritative) |
| `flags` | `u32` | bit 0 dirty, bit 1 needs-check, bit 2 read-only |
| `label` | `char[64]` | UTF-8 volume label, NUL padded |
| `crc32c` | `u32` | CRC32C over the 1020 bytes before it |

A `{block, generation}` pair is read as: "block *n* is the root **only** if the node
stored there carries generation *g*". Stale blocks (lower generation) are ignored.
That single rule is what makes copy-on-write cheap and snapshots free.

### 1.2 Copy-on-write B+tree node (4 KiB)

One node per cluster, never modified in place: a change allocates a new node, writes
it with the *new* generation and only then publishes it through the parent (the last
publish is the superblock). A node is trusted only if its CRC is valid **and** its
self address and generation match what the parent expects.

| Field | Type | Meaning |
|---|---|---|
| `magic` | `char[4]` | `"NFBT"` |
| `level` | `u16` | 0 = leaf |
| `entries` | `u16` | number of entries |
| `generation` | `u64` | written with the node, compared on read |
| `block` | `u64` | self address (detects a block copied to the wrong place) |
| `next` | `u64` | leaf sibling, kept for sequential scans |
| `data_offset` | `u32` | where the entry area starts (40-byte header, padded) |
| `used_bytes` | `u32` | bytes used after `data_offset` (relative, so a node can move) |
| `crc32c` | `u32` | over the whole 4096-byte node with this field zeroed |

* **Leaf** — a packed sequence of serialised PIFT records, each prefixed by a 4-byte
  length. A record is never split across two leaves. Packing beats one-key-per-entry
  here because the record *is* the value and the key (`fileid`) is inside it.
* **Internal** — an array of `{ u64 key_max, u64 child }` pairs (up to 16 per node):
  `child` covers every key `<= key_max`. The walk descends all children in order, so
  the same code works for a scan and for a full walk; `NFS_TREE_MAX_DEPTH` is 8, which
  at 16-wide fan-out and ~1.9 KiB of leaf payload per node is far beyond any realistic
  PIFT (8^16 records).

The namespace ordering itself lives in RAM (the sorted child index, section 2.1), so
the checkpoint tree is purely the on-disk compacted PIFT. Directory links are not a
separate tree: a record names its parent, and the child index is rebuilt from the
records at mount.

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

Append-only log of serialised PIFT records inside the current half,
`pift_first .. pift_first + pift_clusters`. It is **write-through**: every committed change appends
its record(s) here, so the on-disk table always trails the RAM table by at most one
transaction. It is **never read during runtime**: the only readers are

* mount (to rebuild the RAM table), and
* `nextchk` (recovery) when the RAM table cannot be rebuilt or the volume is dirty.

The area is split in **two halves**. Each generation writes only into its own half
(the half selected by `generation & 1`), so the fallback generation always keeps
every record it appended, even after the next checkpoint. When the live half reaches
its high-water mark, it is **compacted**: the whole RAM PIFT is written as a fresh
checkpoint tree, the roots and the half are swapped with a single superblock pair
write, and the half restarts at offset 0. Compaction is itself copy-on-write, so a
crash mid-compaction leaves the previous root valid — and the previous root still has
its own untouched half to read from.

Mount order is: tree of `checkpoint_root` (fall back to `checkpoint_root_prev` and set
the needs-check flag if it does not verify), then the PIFT half of that generation,
then the journal. Records are accepted only if their sequence number is committed, so
reading a half past its valid tail is inert.

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

## 3. Copy-on-write and snapshots

* No metadata block, bitmap block or data page is ever rewritten in place while a
  snapshot may reference it.
* A write to a file allocates new clusters, writes the payload, then atomically
  publishes the new extent list through the PIFT record + journal commit. Old
  clusters are released; those a snapshot still owns stay allocated.
* A snapshot is a **pinned checkpoint generation**: `{tree root, generation}` held
  in a PIFT record (flag `SNAPSHOT`) inside the `.snapshots` directory of the root.
  Creating one checkpoints, then adds a reference to every block the generation
  owns; deleting one drops those references and frees what falls to zero.
* **Refcounts are intervals in RAM, not a tree on disk.** The pin set is a sorted
  array of `{start, length, count}` ranges covering tree nodes (one cluster each)
  and the extent ranges of the records inside them, so its size follows the number
  of extents, not the number of clusters. The set is *derived*: mount rebuilds it by
  walking every snapshot tree, so a crash can never leave the on-disk state and the
  pins disagreeing — there is no second source of truth to keep in step.
* Freeing goes through the pins: `NfsClusterFree` gives back only the clusters no
  pin covers, which is what keeps a pinned generation readable while the live file
  system overwrites and deletes its files.
* Reading a snapshot walks its pinned tree (no cache, no mount): `NfsSnapshotLookup`
  by parent + name, `NfsSnapshotRead` for the data, both through the record's CoW
  extents.
* **Rename, move and replacement** are one PIFT record commit (`NfsRename`), and
  replacement deletes the old target *in the same commit*: no rename-with-delete
  window, no rollback file, no `$RmMetadata`. Cycles are refused by walking the
  parent chain, and a directory with children cannot be replaced by a file.

## 4. What replaces what

| NTFS today | NextFS |
|---|---|
| `$MFT` + `$MFTMirr` | PIFT in RAM; PIFT-on-disk area + PIFT checkpoint tree |
| MFT record `FILE` with attributes | one flat `nfs_pift_record` |
| `$DATA` run lists, resident/non-resident | `extents[]`, plus inline data for small files |
| `$INDEX_ROOT`/`$INDEX_ALLOCATION` (`$I30`) | child index in RAM (sorted), namespace tree on disk |
| `$Bitmap` | copy-on-write bitmap chain |
| `$LogFile` | journal ring, commit-frame protocol (section 2.2) |
| `$UsnJrnl`, indexed search, `$Secure`, `$UpCase` | nothing: name comparison uses a casefold table compiled in (`nextfslib/casefold`), security descriptors live in the PIFT record tail (later phase) |
| `fs_rec` OEM "NTFS    " | OEM "NEXTFS  " + superblock magic |

---

## 5. Open questions (to settle before phase 10)

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

## 6. Implementation status (v0.9, branch `nextfs`)

Done and tested in this phase:

| Piece | Files | Verified by |
|---|---|---|
| on-disk format, superblock, bitmap | `nextfslib/nextfs_super.c` | format + mount + remount in the self-test |
| PIFT record codec + RAM index | `nextfslib/nextfs_pift.c` | create/lookup/readdir/unlink checks |
| journal, commit frames, replay | `nextfslib/nextfs_journal.c` | "crash after commit" and "torn commit frame" cases |
| transactions, checkpoint chain | `nextfslib/nextfs_volume.c` | remount after clean unmount, free-space accounting |
| mkfs / inspector / checker | `nextfslib/tools/nextformat.c`, `nextfsinfo.c`, `nextchk.c` | 16 and 32 MiB images formatted, listed, checked, corrupted and repaired |
| any-offset CoW writes, holes, truncate | `nextfslib/nextfs_volume.c` | boundary-spanning writes, sparse reads, truncate down/up |
| sorted child index | `nextfslib/nextfs_pift.c` | 300-file directory, binary-search lookup, ordered enumeration |
| multi-level checkpoint tree | `nextfslib/nextfs_tree.c` | 8000 files: root is internal (level 2), remount, enumerate, lookup |
| two-generation roots + PIFT halves | `nextfslib/nextfs_volume.c` | 6 checkpoints with zero cluster drift; corrupt live root ⇒ fallback mount |
| driver skeleton | `drivers/filesystems/nextfs/nextfs.c` | not compiled yet (needs the RosBE build) |

`nextfslib/tests/nextfs_selftest.c` runs 310 checks: format, mount, create/write/
read, mkdir, nested file, append, readdir, unmount+remount (PIFT rebuilt from
disk only), crash after the commit point (change must survive), crash before the
commit frame (change must be discarded), delete, free-space return, unaligned
overwrites, writes across a cluster boundary, sparse holes, truncate down and up,
a 300-file directory with a multi-node checkpoint, 400 randomised writes compared
byte for byte with a reference buffer, a crash in the middle of a 40-write burst that
must recover to a whole step (never a torn write), 8000 files forcing a three-level
checkpoint tree, six back-to-back checkpoints that must not drift the free-cluster
count, a deliberately corrupted live root that must be mounted through the
fallback generation with the needs-check flag raised, and snapshots: the pinned
generation keeps the original bytes through an in-place overwrite, a live delete
and a remount, the pins come back at mount, two snapshots do not disturb each
other, and a 128-cluster file the live tree deleted comes back to the allocator
only when the snapshot that pinned it is dropped. Group [21] covers rename, move,
replacement, cycle refusal, attributes and both restore paths (overwritten file,
deleted file). Group [22] covers path resolution and every disposition. Group
[23] covers handles: position tracking, clamps, access modes and the closed-handle
refusal. Group [24] covers wildcards, enumeration cookies, the information query
and delete-on-close (including the refused non-empty directory). Group [25] is
the corruption loop described above.

Landed in phase 2 (`nextfslib`):

* **Any-offset writes, copy-on-write.** `NfsWriteRange` maps the affected cluster
  range to fresh clusters, carries over the bytes a partially covered cluster
  keeps, publishes the new extent list and only then releases the replaced
  clusters. Middle-of-file writes, unaligned writes across a cluster boundary and
  sparse writes past EOF all work; the gap reads as zeroes.
* **Truncate** (`NfsTruncate`): shrinking rewrites the boundary cluster with a
  zeroed tail (copy-on-write) before releasing the tail clusters, so a later
  growth cannot resurrect stale bytes.
* **Sorted child index in RAM** (`NFS_CHILD_SET`): per-directory arrays sorted by
  case-folded name, binary-searched by `NfsLookup`, used directly by
  `NfsReadDir` and updated transactionally. No disk read is involved in a lookup.
* **Checkpoints scale**: the chain now holds up to 4096 nodes and the self-test
  exercises 300 files across several nodes, including remount.

Landed in phase 9 (robustness, no new API):

* **Random-corruption test**: 40 rounds, each damaging a fresh copy of a live
  image in 1–16 random bytes (weighted towards the metadata region), then mounting
  it. Every round must either mount and survive a lookup, a read and two directory
  enumerations, or fail with a clean error code — never crash, never hang. The
  suite runs green under AddressSanitizer **and** UndefinedBehaviorSanitizer.
* **Superblock backup**: with the 1024-byte primary superblock zeroed by hand, the
  volume still mounts from the second copy and the files are still reachable.
* The same loop proves the damage does not leak into the next round: after it, the
  pristine image mounts again.

Landed in phase 8 (`nextfslib`, `nextfs_namespace.c` + `nextfs_handle.c`):

* **NT wildcards** (`NfsMatchPattern`): `*` and `?`, case-insensitive, with the
  single-backtrack match so `a*b*c` works without recursion, plus the DOS rule NT
  keeps — `*.*` also matches a name with no extension.
* **Enumerations with a cookie** (`NfsEnumerate`): `IRP_MJ_DIRECTORY_CONTROL` asks
  for one entry at a time and resumes from an index; the cookie is the position in
  the directory's child set, and the pattern is applied as a filter.
* **`NfsQueryStandardInfo`**: creation/access/write/change times, allocation size,
  end of file, attributes, link count and the directory flag — one call for
  `FileBasicInformation`, `FileStandardInformation` and `FileAllInformation`.
* **Delete-on-close** (`NfsHandleSetDeleteOnClose` + close): the file goes when the
  last handle closes, which is how `FileDispositionInformation` is implemented; a
  directory that still has children refuses instead of leaking them.

Landed in phase 7 (`nextfslib`, `nextfs_handle.c`):

* **File handles** (`NFS_HANDLE`): a per-volume table of `{fileid, position,
  access, flags}` slots. A handle is an **index**, never a pointer, because a
  write-through commit appends to the record array and can move anything the
  record's neighbours live in — exactly the bug that makes a naive FSD crash.
* Handles own the file pointer: `NfsHandleRead`/`Write` advance it,
  `NfsHandleSeek` clamps at EOF the way NT does, `NfsHandleSetEndOfFile` truncates
  and pulls a position past the new size back. Access mode is enforced per handle
  (a read-only handle cannot write, a directory handle cannot read data but
  enumerates), and a closed handle is refused on every call.
* This is the object `nextfs.sys` will keep per `FILE_OBJECT` in
  `FsContext2`, so the IRP handlers stay a mapping table onto nextfslib.

Landed in phase 6 (`nextfslib`, `nextfs_namespace.c`):

* **`NfsOpenPath`**: backslash or slash paths from the root, `.` and `..` (which
  stops at the root instead of escaping), component length and character limits
  (wildcards, `:`, `|` and the rest of the NT set are refused), and the six
  creation dispositions `FILE_SUPERSEDE / OPEN / CREATE / OPEN_IF / OVERWRITE /
  OVERWRITE_IF` — the exact set `IRP_MJ_CREATE` hands to a file system. The object
  found must be of the kind asked for: a directory does not open as a file.
* **`NfsQueryVolumeInfo`**: cluster size, total and free bytes, generation, file
  and snapshot counts, label, UUID — one call for `FileFsSizeInformation`,
  `FileFsVolumeInformation` and `FileFsAttributeInformation`.
* Locking, oplocks, security contexts, reparse points and byte-range locks are
  deliberately absent: they belong to the FSD phase, together with the I/O paths.

Landed in phase 5 (`nextfslib`, `nextfs_namespace.c`):

* **`NfsRename`**: rename in place, move across directories, and atomic replacement
  (`MOVEFILE_REPLACE_EXISTING`): the deleted target and the renamed source go into
  one commit, so a crash cannot leave both names or neither.
* **`NfsSetFileInfo`**: attributes and the four timestamps, one record commit.
* **`NfsRevertFromSnapshot`**: restores a file's bytes, size, attributes and
  timestamps from a pinned generation, and re-creates the file under its old name
  if the live tree had deleted it.

Landed in phase 4 (`nextfslib`, `nextfs_snapshot.c`):

* **Snapshots** create/delete/list/read. A snapshot pins the current checkpoint
  generation; `nextfsinfo --snapshot v1 --cat-snapshot v1 notes.txt` shows the
  original bytes of a file the live tree has already overwritten or deleted, also
  in a later process (the pins are rebuilt at mount).
* **Pin set (refcounts)** as sorted intervals with splitting on partial overlaps.
  `NfsPinFreeGaps` is on the free path, so no allocation path can take a cluster a
  snapshot owns; releasing a snapshot frees only what neither it nor the live
  namespace still maps.
* **Write-through area fix found by the tests**: a fallback mount used to leave
  `pift_head` at the end of the half, so the next commit failed with `ENOSPC`
  before it could checkpoint. `NfsPiftScan` now puts the append pointer back to the
  real tail, and `NfsCommitRecords` checkpoints *before* writing when the area is
  past its high-water mark.
* **Snapshot records** carry `snapshot_root` and `snapshot_generation`; format
  version 3.

Landed in phase 3 (`nextfslib`):

* **Multi-level checkpoint B+tree** (`nextfs_tree.c`). Leaves are packed serialised
  records, internal levels are `{key_max, child}` arrays, every node carries its own
  address, generation and CRC32C. `NfsTreeBuild` writes the whole tree
  copy-on-write, `NfsTreeWalk` reads it back at mount, `NfsTreeCollect` lists the
  blocks of a generation so the retired one can be freed. Checkpoint publishes with
  one superblock pair write; the old blocks are freed afterwards.
* **Two live generations.** `checkpoint_root_prev` keeps the generation before, so a
  tree that does not verify is not a catastrophe any more: mount uses the fallback,
  raises `NFS_SB_NEEDS_CHECK` and `Stats.UsedFallbackRoot`. A new checkpoint retires
  the generation *before* the fallback, which is why the free-cluster count is stable.
* **Two PIFT halves.** The on-disk PIFT is now two halves, one per live generation,
  so the fallback generation still has every record it appended. Failing over also
  switches `pift_first` to that half.
* **`nextchk`** (`tools/nextchk.c`): verifies both superblocks, walks the checkpoint
  tree node by node (magic, self address, CRC, generation), reports the leaks and the
  metadata clusters the bitmap thinks are free, cross-checks every record's extents
  against the volume size, and with `--repair` rebuilds the bitmap from the PIFT and
  checkpoints a clean generation. A plain check is read-only: it puts the superblock
  flags back exactly as it found them.

Still open, in order of importance:

1. **Kernel I/O paths** (`IRP_MJ_CREATE/READ/WRITE/DIRECTORY_CONTROL`, cache
   manager callbacks). Until they exist, `nextfslib` is not in
   `sdk/lib/fslib/CMakeLists.txt` and `nextfs.sys` is not in
   `drivers/filesystems/CMakeLists.txt`: NTFS stays the boot file system.
2. **Security descriptors, compression, encryption, quotas**: not implemented.
   Names are compared with a folded table compiled in (`$UpCase` equivalent);
   ACLs will land in the PIFT record tail as a deduplicated blob.
