# Vaultik repository format (version 1)

This document is the source of truth for the on-disk format. Code must follow it; changes need a
format version bump.

All integers are little-endian. `varint` is unsigned LEB128 (7 bits per byte, high bit = more).
`string` is `varint length` + UTF-8 bytes. `Id` is 32 raw bytes, written as 64 lowercase hex chars
in file names.

## Layout

```
<repo>/
  config                    plaintext header + password-wrapped master key
  recovery                  optional: the master key wrapped with the recovery key
  data/<xx>/<pack-id>       pack files (xx = first two hex chars of the pack id)
  index/<index-id>          sealed index files
  snapshots/<snapshot-id>   sealed snapshot manifests
  locks/<lock-id>           sealed lock files of the runs using the repository
  parity/percent            optional: repair data is on, at this percentage ("5\n"), see below
  parity/<xx>/<pack-id>     optional: the repair data of a pack (see "Recovery data")
```

Files are written to a temporary name and renamed into place, so a reader never sees a partial
file. In S3-compatible cloud storage (`s3:https://<endpoint>/<bucket>[/<folder>]`) each file is
one object named `<folder>/<path>` with the same layout; object uploads are atomic, so there are no
temporary names. The cloud drives (`pcloud:`, `onedrive:`, `gdrive:`, `mega:` followed by a
folder) hold the same layout as real folders and files; an upload either replaces a file whole or
leaves it as it was. A repository folder copied to or from a bucket or drive stays valid. Write order during a backup is: packs, then the index files that reference them, then the
snapshot. A crash leaves at worst unreferenced packs, never a snapshot pointing at missing data;
the next prune removes them. Lock files (see below) were added in Vaultik 0.2 without a format
version change: older versions ignore them.

## Keys

The master key is 64 random bytes: `enc_key` (32, AES-256-GCM) followed by `id_key` (32, keyed
BLAKE3). It never touches disk in plaintext.

The password is stretched with PBKDF2-HMAC-SHA256 (`kdf_iterations`, 32-byte `salt`) into a
32-byte key-encryption key, which seals the master key with AES-256-GCM. The AAD of that seal is
the full config header (everything before the wrapped key), so tampering with any parameter fails
authentication.

Changing the password rewrites only `config`, with a new salt; the master key, and with it every
other file, stays the same.

The optional `recovery` file (added in Vaultik 0.3 without a format version change; older
versions ignore it) wraps the same master key with a recovery key: 20 random bytes, shown to the
user as 32 characters of Crockford base32 (`0-9 A-Z` without `I L O U`) in groups of 4. Input is
read case-insensitively with `O` as `0` and `I`/`L` as `1`. The raw 20 bytes go through
PBKDF2-HMAC-SHA256 (`iterations`, `salt` below) to the key-encryption key. Making a new recovery
key replaces the file.

```
magic            8 bytes  "VKRECOV\0"
version          u32      1
repo_id          16 bytes the repo_id from config
iterations       u32      100000
salt             32 bytes
--- end of header (AAD) ---
wrapped_len      u32      92
wrapped_key      nonce(12) || ciphertext(64) || tag(16)
```

The AAD binds the file to its repository: a recovery file copied from another repository fails
authentication.

## config

```
magic            8 bytes  "VAULTIK\0"
version          u32      1
repo_id          16 bytes random
kdf              u32      1 = PBKDF2-HMAC-SHA256
kdf_iterations   u32      default 600000
salt             32 bytes
chunk_min        u32      default 262144   (256 KiB)
chunk_avg        u32      default 1048576  (1 MiB, power of two)
chunk_max        u32      default 4194304  (4 MiB)
zstd_level       u32      default 3
--- end of header (AAD) ---
wrapped_len      u32      92
wrapped_key      nonce(12) || ciphertext(64) || tag(16)
```

## Sealed objects

Every encrypted object (blob, index file, snapshot) uses the same envelope:

```
sealed    = nonce(12) || AES-256-GCM(enc_key, nonce, plaintext, aad) || tag(16)
plaintext = compression u8 (0 = none, 1 = zstd) || varint raw_len || payload
```

Nonces are 96 random bits per object. With 1 MiB chunks, the birthday bound for nonce collision
reaches 2^-32 only after about 2^32 objects (around 4 PiB), far beyond a personal repository.

Compression is applied only when it makes the payload smaller.

AAD values bind each object to its role and name:

| Object        | AAD                                       |
|---------------|-------------------------------------------|
| blob          | `type u8` || `blob id` (32)               |
| pack header   | `"VKPKHDR1"`                              |
| index file    | `"VKINDEX1"` || index id (32)             |
| snapshot      | `"VKSNAP01"` || snapshot id (32)          |
| lock          | `"VKLOCK01"` || lock id (32)              |

## Blobs

A blob is either file content (`type 0`, data) or a directory listing (`type 1`, tree). The blob id
is `BLAKE3-keyed(id_key, type u8 || plaintext)`. Keying hides content fingerprints from whoever
stores the repository (for example a cloud provider); the type prefix keeps data and tree blobs in
separate id spaces, so a file chunk that happens to equal a tree encoding is never confused with it. Identical plaintext always gets the same id, which is
what makes deduplication work across files, folders, snapshots and machines that share a password.

## Chunking

File content is split with FastCDC (normalized chunking, level 2):

- Rolling hash: `h = (h << 1) + gear[byte]` over 64-bit values.
- Bytes before `chunk_min` are skipped; a cut is never made before `chunk_min` or after `chunk_max`.
- Between `chunk_min` and `chunk_avg` a cut is made when `h & mask_s == 0`; after `chunk_avg` when
  `h & mask_l == 0`. With `b = log2(chunk_avg)`, `mask_s` is the top `b + 2` bits and `mask_l` the
  top `b - 2` bits of a 64-bit word.
- `gear` is 256 values of splitmix64 starting from seed `0x5641554C54494B31` ("VAULTIK1"):
  `x += 0x9E3779B97F4A7C15; z = x; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;`
  `z = (z ^ (z >> 27)) * 0x94D049BB133111EB; gear[i] = z ^ (z >> 31)`.

Chunking restarts at every file, so a file smaller than `chunk_min` is a single chunk.

## Pack files

```
blob_0 .. blob_n-1        sealed blobs, back to back
header                    sealed (AAD "VKPKHDR1"), plaintext = varint count, then per blob:
                            type u8, id (32), varint offset, varint length, varint raw_len
header_len                u32
magic                     4 bytes "VKPK"
```

The pack id is the unkeyed BLAKE3 hash of the whole pack file. Packs are closed at about 32 MiB.
The header makes every pack self-describing, so the index can be rebuilt from packs alone.

A pack may hold blobs of both types. While the storage keeps file data in an archive storage
class (S3 `x-amz-storage-class`, the job's "Storage class"), data blobs and tree blobs go to
separate packs: packs holding only data blobs, and their `parity/<xx>/<pack-id>` files, are
uploaded in that class; packs holding tree blobs and every other file stay in the standard class,
so the index, snapshots and folder listings can be read without a thaw. The format is the same
either way, and a reader does not need to know which packs are archived (reading one that is
not thawed fails until it is).

## Index files

Sealed with AAD `"VKINDEX1" || index id`. Plaintext:

```
version   u32  1
packs     varint
per pack: pack id (32), varint count, then per blob the same entry layout as the pack header
```

## Trees

A tree blob lists one directory. Entries are sorted by name, bytewise on UTF-8, with no
duplicates. Writers produce version 3 when any node has extras (see below) and version 2
otherwise, so trees without extras stay readable by Vaultik 0.5 and older; readers accept
versions 1 to 3.

```
version   varint 2 or 3
count     varint
per node:
  name        string
  kind        u8       0 = file, 1 = directory, 2 = metadata only (no content), 3 = symlink
  attributes  u32      Win32 file attributes (0 when backed up on Linux)
  mode        u32      POSIX permission bits, 07777 (0 when backed up on Windows)
  uid         u32      POSIX owner (0 on Windows)
  gid         u32      POSIX group (0 on Windows)
  ctime       u64      Windows: creation time. Linux: status change time. FILETIME units
                       (100 ns since 1601-01-01 UTC)
  mtime       u64      last write time, FILETIME units
  size        u64      file size in bytes (0 for everything but files)
  file:       varint chunk count, then chunk ids (32 each), in file order
  directory:  subtree id (32)
  symlink:    string   link target, exactly as stored in the link
  extras:     version 3 only: varint count, then per extra: tag u8, varint length, that many bytes
```

Extras (readers skip tags they do not know):

| Tag | Content | Meaning |
|---|---|---|
| 1 | id (32) | a Data blob holding the Windows security descriptor (owner, group, access list; self-relative). Identical descriptors are one blob. |
| 2 | name (string), size (varint), chunk count (varint), chunk ids (32 each) | an NTFS alternate data stream of the file ("file:name") |
| 3 | reparse tag (u32), flags (u8, bit 0 = relative), target (string) | the target of a Windows symlink (`0xA000000C`) or junction (`0xA0000003`) on a kind 2 node; absolute targets without the `\??\` prefix |

Every blob an extra names counts as in use for prune, is copied with the version, and is checked
by verify like file content.

Version 1 (written by Vaultik 0.1, Windows only) has the same layout without `mode`, `uid` and
`gid`, and only kinds 0 to 2.

Kind 2 is used for Windows symlinks, junctions and cloud placeholders (OneDrive files that are
not on the disk), and for Linux devices, sockets and pipes. Restores recreate symlinks and
junctions that have a link extra (without following them) and skip the rest.

ctime is only compared for change detection between backups; restores set it on Windows and
ignore it on Linux, where it cannot be set.

The snapshot root tree is virtual. On Windows its children are drive letters (`C`) or `UNC` for
network paths; on Linux they are the top-level folders (`home`, `etc`, ...). Below those come
the path components down to each backed-up source. Virtual directories have zero attributes,
mode and timestamps.

## Snapshots

Sealed with AAD `"VKSNAP01" || snapshot id`. The snapshot id is 32 random bytes. Plaintext:

```
version       u32     1
time          u64     FILETIME UTC
hostname      string
username      string
paths         varint count, then strings (full source paths as given)
tree          Id      root tree
has_parent    u8      0 or 1, followed by the parent snapshot id (32) when 1
tags          varint count, then strings
stats         varint files, dirs, bytes, new_bytes, stored_bytes, errors, skipped
```

Tags starting with `vaultik:` are reserved: they carry fields added after version 1 without a
format change, so older Vaultik versions still read the snapshot (and show them as tags):

- `vaultik:pinned`: retention and size limits never remove the snapshot.
- `vaultik:note=<text>`: the user's description of the snapshot.
- `vaultik:changes=<new>,<changed>,<removed>`: files new and changed since the parent snapshot,
  and files or folders of the parent that are gone (Vaultik 0.4 and later).
- `vaultik:copy-of=<64 hex>`: in a second copy, the id of the snapshot it was copied from.
- `vaultik:apps=<64 hex>`: the id of a Data blob listing the programs installed when the snapshot
  was made (Vaultik 0.7 and later; see below).

The program list blob is stored like file content (so an unchanged list is one blob across
snapshots). Its plaintext is UTF-8 JSON, sorted by name:

```
{"version":1,"apps":[{"name":"7-Zip 24.08 (x64)","version":"24.08","publisher":"Igor Pavlov",
                      "id":"7zip.7zip","source":"winget"}, ...]}
```

`name` is required; `version`, `publisher`, `id` (a winget package id: 1 to 128 characters of
`A-Z a-z 0-9 . _ - +`, starting with a letter or digit) and `source` (`winget` or `msstore`) are
left out when unknown. Readers drop an `id` or `source` that does not match these rules. Prune
counts the blob as in use, copy copies it with the snapshot, verify reports it when missing, and
repair saves the snapshot again without the field when the blob is lost. Versions before 0.7 do
not know the tag: their prune removes the blob, and the snapshot then reads as having no list.

Unknown reserved tags are ignored. Pinning or describing a snapshot rewrites its file under the
same id while holding a shared lock, and never recreates a snapshot that was removed.

## Locks

Every run that reads or writes snapshot data holds a lock file. The lock id is 32 random bytes;
the file is sealed with AAD `"VKLOCK01" || lock id` (never compressed). Plaintext:

```
version     u32     1
exclusive   u8      0 = shared, 1 = exclusive
time        u64     FILETIME UTC of the last refresh
pid         u32     process id of the holder
hostname    string
username    string
command     string  "backup", "prune", ...
```

Backup, restore, ls and verify take a shared lock; forget and prune take an exclusive one,
because they delete objects. A run writes its own lock first and then lists the others; on a
conflict it removes its lock again and waits or gives up. Holders rewrite their lock every 5
minutes. A lock not refreshed for 30 minutes, or held by a process that no longer runs on the
same computer, is stale: it is ignored and deleted by whoever finds it.

## Prune

Prune holds the exclusive lock and works in this order, so a crash at any point leaves a valid
repository: (1) collect every blob reachable from any snapshot, stopping without changes if a
snapshot or folder listing cannot be read; (2) write new packs with the blobs still in use from
packs that are mostly unused (sealed blobs are copied as they are, after authenticating them);
(3) write fresh index files for every pack that stays; (4) delete the old index files; (5) delete
the packs nothing uses any more. Packs that no index file lists are read through their own
header: kept and indexed again if they hold blobs in use, deleted otherwise.

## Repair

Repair (`vaultik repair`) holds the exclusive lock and opens the repository without reading the
index files. In this order, so a crash at any point leaves a repository that opens: (1) read
every pack's header and (unless `--quick`) decode and hash-check every blob; the first intact
copy of each blob goes into the new index, packs that cannot be read are left in place and not
indexed; (2) write new index files for that index, then delete every old index file; (3) delete
snapshot files that cannot be opened or whose root tree is lost; for the others, rewrite every
tree that references a lost blob (dropping the file nodes whose chunks are missing and the
directory nodes whose tree is missing), save the snapshot again with the new root tree and a note
"(repaired: N file(s) and M folder(s) were lost)", then delete the old snapshot file.

## Repair data

While `parity/percent` exists (a decimal 1 to 50), every program that writes a pack also writes
`parity/<xx>/<pack-id>`: the pack's recovery data (below) at that percentage, from the pack as
written. It is plaintext: it is computed over the pack file, which is ciphertext already, and the
hashes in it are of that ciphertext. Writing it is best effort (a pack without it is valid); a
health check that reads an intact pack without it adds it. Prune deletes it with its pack. Repair
rebuilds a pack that cannot be read whole, whose content does not hash to its id, or whose header
cannot be read, from its repair data, and when the result hashes to the pack id it writes the
pack back at once (the content its id names, so nothing that refers to it changes). Removing
`parity/percent` turns it off for new packs; existing repair data stays usable. Versions before
0.7 ignore the `parity/` folder.

## Recovery data

Recovery data lets a damaged file (usually a pack file) be rebuilt, like a `.par2` file. The file
of `data_size` bytes is cut into `k` data shards of `shard_size` bytes, back to back, the last one
zero-padded; `m` parity shards are computed with a systematic Reed-Solomon code over GF(2^8)
(polynomial `0x11D`). Any `k` intact shards out of the `k + m` rebuild the file, and since the
shards are contiguous, a run of bad sectors damages only the few shards it touches. Little-endian:

```
magic          4 bytes "VKRD"
version        u32  1
data_size      u64  size of the original file
shard_size     u32
data_shards    u32  k
parity_shards  u32  m
data_hash      32   BLAKE3 of the whole original file
shard_hashes   16 * (k + m)  first 16 bytes of BLAKE3 of each data shard (padded), then of each
                             parity shard
header_hash    16   first 16 bytes of BLAKE3 of every byte above
parity         m * shard_size  parity shard 0 .. m-1, back to back
```

- Parity shard `i` is the sum (XOR) over data shards `j` of `C[i][j] * shard_j`, byte by byte,
  with the Cauchy matrix `C[i][j] = 1 / ((k + i) xor j)` in GF(2^8).
- The writer takes a percentage `p` (1 to 50) and chooses: `k_max` = the largest value up to 240
  with `k_max + ceil(k_max * p / 100) <= 255`; `shard_size = max(ceil(data_size / k_max),
  min(data_size, 4096))`; `k = ceil(data_size / shard_size)`; `m = max(1, ceil(k * p / 100))`. An
  empty file has `k = 1` and `shard_size = 0`.
- Readers reject (as "not recovery data") a wrong magic or version, a header checksum mismatch,
  `k` or `m` of 0, `k + m > 255`, `data_size` above 1 GiB, and any shard layout the writer cannot
  produce: for `data_size > 0`, `0 < shard_size <= data_size` and
  `(k - 1) * shard_size < data_size <= k * shard_size`. Parity shards that fail their hash or lie
  past the end of the recovery data are not used.
- Repair hashes every data shard of the damaged file (bytes past the end of a cut-short file read
  as zeros, bytes past `data_size` are dropped), rebuilds the shards that do not match from the
  intact data and parity shards, and keeps the result only if its BLAKE3 equals `data_hash`.
