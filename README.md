# tgzcat

List members of tar.gz/tgz archives or stream a selected member to standard output.
Supports GNU/PAX long names, substring searches, and compressed caches for both
listing and indexed extraction.

Build and install to $HOME/bin:

    make
    make install

Requires GCC, make, and zlib development headers/libraries. The default fast
reader also requires Rust/Cargo. Dependencies are pinned in Cargo.lock; after
the first build, make CARGO_FLAGS=--offline works without network access.
To build with GCC and system zlib only:

    make BACKEND=zlib
    make install BACKEND=zlib

The C program uses flate2/zlib-rs for ordinary streaming reads in the fast build.
Index construction and indexed extraction use zlib block boundaries, bit priming,
and dictionary restoration in both builds.

Basic syntax (options may appear before or after the archive):

    tgzcat ARCHIVE [MEMBER] [OPTIONS]

Without MEMBER, list filenames. With MEMBER, write its contents to standard
output. A name without a slash matches the basename; otherwise it matches the
full archive path. If names repeat, the first regular-file match is extracted.

| Option | Action |
| --- | --- |
| -h, --help | Show usage and examples; running without arguments does the same. |
| -l, --list | List filenames explicitly (the default without MEMBER). |
| -f, --find TEXT | List names containing a literal, case-sensitive substring. |
| -1, --first TEXT | List only the first matching name. |
| -c, --cache | Use or build compressed caches for listing and extraction. |
| --cache release | Remove only the specified archive caches. |
| -v, --verbose | Report cache hits, index building, and extraction restart positions to stderr. |
| -- | Treat remaining arguments as paths, including names starting with a dash. |

--find=TEXT, --first=TEXT and --cache=release are also accepted.
MEMBER extraction may use --cache, but cannot be combined with --list, --find
or --first. --find/--first output filenames, not file contents.
Exit status is 0 for success, 1 for no match or an I/O error, and 2 for invalid
arguments. Help is written to standard output.

Examples:

```sh
# Show help and examples.
tgzcat

# List all archive members.
tgzcat I26283X.tar.gz

# Read a DRG text file without creating an extracted file.
tgzcat I26283X.tar.gz I26283X.DRG | less

# Extract a cor file using its full archive path.
tgzcat observation.tgz experiment/cor/scan.cor > scan.cor

# Search names, or stop after the first matching name.
tgzcat I26283X.tar.gz --find YAMAGU32_YAMAGU34
tgzcat I26283X.tar.gz --first .DRG

# First full scan builds a filename cache and an extraction index.
tgzcat I26283X.tar.gz --cache > /dev/null

# Repeated extraction seeks near the member; verbose shows the restart offset.
tgzcat I26283X.tar.gz I26283X.DRG --cache --verbose

# An extraction can also build the index on its first invocation.
tgzcat observation.tgz scan.cor --cache > scan.cor

# Search the cached list; short options and options-first syntax also work.
tgzcat I26283X.tar.gz --cache --find YAMAGU32_YAMAGU34
tgzcat -c -f YAMAGU32_YAMAGU34 I26283X.tar.gz

# Use grep when a regular expression is needed.
tgzcat I26283X.tar.gz --cache | grep "YAMAGU32_YAMAGU34.*[.]cor$"

# Remove this archive's list, index, and abandoned cache temporary files.
tgzcat I26283X.tar.gz --cache release

# A dash-prefixed archive name.
tgzcat -- -observation.tar.gz
```

Without --cache, the archive is read sequentially and no cache is created.
With --cache, the first full scan records every member's name, payload offset
and size, plus restartable DEFLATE positions about every 16 MiB of uncompressed
data. Restart points are at eligible block boundaries, so gaps can be larger.
Each point stores a compressed-file offset, bit count, and up to 32 KiB of history.

Later extraction reads the compressed index, locates the first matching member,
seeks the archive near that member, restores the history and bits, and decompresses
only the gap and requested contents. It does not decompress from the archive start.
Plain .tar files use direct byte offsets without DEFLATE dictionaries.
Concatenated gzip members are supported, including requests crossing their boundaries.

A first extraction with --cache continues scanning after emitting the requested
file so it can finish and validate the index. It can therefore take longer than
an uncached extraction on that first call. Subsequent calls reuse the index.
--first uses an existing cache but stops early without building one on a miss.
A filtered --find cache build still records all names and members.

Caches are opt-in and use one directory, in this precedence order:

- TGZCAT_CACHE_DIR, if set
- $XDG_CACHE_HOME/tgzcat, if XDG_CACHE_HOME is an absolute path
- $HOME/.cache/tgzcat

Each archive can have two files sharing the hash of its canonical absolute path:

- <path-hash>.list.z: small zlib-compressed filename list, with a 64-byte metadata header.
- <path-hash>.index.gz: gzip-compressed member positions and restart dictionaries.

The index contains metadata and short decompression histories, not copies of
complete member contents. Its size grows with the number of checkpoints:
roughly 32 KiB per 16 MiB before compression, plus member metadata.
Readers use fixed-size buffers and retain only a few checkpoints at a time;
index construction streams checkpoints to disk rather than keeping all in RAM.
If a filename cache is missing, it can be recreated from a valid extraction index
without scanning the archive again.

Archives with the same basename in different directories have separate caches.
Symlink aliases share a cache. Replacing an archive overwrites the corresponding
cache paths. Device, inode, size, modification time and change time are checked
before reuse. Compressed indexes are validated before extraction starts.
Whole-archive gzip CRCs are checked during index construction. Indexed extraction
relies on unchanged archive metadata and does not rescan the archive to recheck
the whole-stream CRC. Uncached extraction and --first also stop before validating
the unread remainder.

Old filename-only caches are upgraded automatically on the next full --cache
invocation. Corrupt or outdated caches are rebuilt. If cache creation is not
possible, ordinary scanning/extraction still works.

Release is archive-specific and idempotent:

    tgzcat observation.tar.gz --cache release
    tgzcat --cache release observation.tar.gz

It removes that archive's list, index and their temporary files, preserving other
archives, unrelated files and the cache directory. A bare tgzcat --cache release
is rejected. A recognized legacy .tgzcat-list beside the specified archive is
also removed; there is no global cache deletion command.

Run tests (Python 3 required):

    make check
    make check BACKEND=zlib

Checks cover GNU/PAX names, duplicate basenames, zero-length members, non-byte-aligned
restart points, dictionary restoration, concatenated gzip streams, corrupt indexes,
source invalidation, broken pipes, and sparse tar offsets beyond 4 GiB.

To measure a real archive (test caches stay in a temporary directory):

    make check ARCHIVE=/path/to/observation.tar.gz
    make check ARCHIVE=/path/to/observation.tar.gz BASELINE=/path/to/old-tgzcat

On I26283X.tar.gz (220 MiB, 691 names), medians of three runs were:

| Extraction | Uncached | Indexed |
| --- | ---: | ---: |
| I26283X.DRG | 0.680 s | 0.014 s |
| late YAMAGU32_YAMAGU34 cor (1.2 MiB) | 0.707 s | 0.011 s |

The initial cache build took 1.34 s. Storage was 3,013 bytes for the list plus
427,852 bytes for the index (22 checkpoints), about 421 KiB total. Indexed peak
RSS was about 3.3-3.5 MiB. Sizes and timings depend on archive contents and host.

Third-party sources and licenses:

- https://github.com/rust-lang/flate2-rs (LICENSE.flate2-MIT / LICENSE.flate2-APACHE)
- https://github.com/trifectatechfoundation/zlib-rs (LICENSE.zlib-rs)
- https://github.com/madler/zlib/blob/master/examples/zran.c (checkpoint algorithm; LICENSE.zran)

The checkpoint reader in seek_index.h is a streaming adaptation of the zran
approach; it does not retain an array of all decompression dictionaries.
