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
  data/<xx>/<pack-id>       pack files (xx = first two hex chars of the pack id)
  index/<index-id>          sealed index files
  snapshots/<snapshot-id>   sealed snapshot manifests
```

Files are written to a temporary name and renamed into place, so a reader never sees a partial
file. Write order during a backup is: packs, then the index files that reference them, then the
snapshot. A crash leaves at worst unreferenced packs, never a snapshot pointing at missing data.

## Keys

The master key is 64 random bytes: `enc_key` (32, AES-256-GCM) followed by `id_key` (32, keyed
BLAKE3). It never touches disk in plaintext.

The password is stretched with PBKDF2-HMAC-SHA256 (`kdf_iterations`, 32-byte `salt`) into a
32-byte key-encryption key, which seals the master key with AES-256-GCM. The AAD of that seal is
the full config header (everything before the wrapped key), so tampering with any parameter fails
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

## Index files

Sealed with AAD `"VKINDEX1" || index id`. Plaintext:

```
version   u32  1
packs     varint
per pack: pack id (32), varint count, then per blob the same entry layout as the pack header
```

## Trees

A tree blob lists one directory. Entries are sorted by name, bytewise on UTF-8, with no
duplicates. Writers produce version 2; readers accept versions 1 and 2.

```
version   varint 2
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
```

Version 1 (written by Vaultik 0.1, Windows only) has the same layout without `mode`, `uid` and
`gid`, and only kinds 0 to 2.

Kind 2 is used for Windows symlinks, junctions and cloud placeholders (OneDrive files that are
not on the disk), and for Linux devices, sockets and pipes. Restores skip these.

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
