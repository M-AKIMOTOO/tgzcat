# tgzcat

List members of tar.gz/tgz archives or stream a selected member to standard output.
Supports GNU/PAX long names, substring searches and an optional compressed list cache.

Requires GCC, make, and zlib development headers/libraries. The default fast build
also requires Rust/Cargo; tests require Python 3.

Build and install to $HOME/bin:

    make
    make install

The C main program is compiled with GCC. By default a streaming reader using
flate2 and zlib-rs is built with Rust/Cargo and linked statically. Dependencies
are pinned in Cargo.lock. Initial builds may download these two dependencies;
later offline builds work with make CARGO_FLAGS=--offline.
The fast reader uses 256 KiB input and 1 MiB output buffers and does not retain
the full archive in RAM. Unselected payloads are consumed directly from the
output buffer without copying them to another buffer.

To build with GCC and system zlib only (no Rust toolchain):

    make BACKEND=zlib
    make install BACKEND=zlib


Basic syntax (options may appear before or after the archive):

    tgzcat ARCHIVE [MEMBER] [OPTIONS]

With no MEMBER, list filenames. With MEMBER, write its binary contents to standard
output. If MEMBER contains no slash, match its basename; otherwise match the
full archive path. Duplicate names select the first match.

| Option | Action |
| --- | --- |
| -h, --help | Show usage and examples; no archive needed. |
| -l, --list | List filenames explicitly (the default without MEMBER). |
| -f, --find TEXT | List names containing a literal, case-sensitive substring. |
| -1, --first TEXT | List only the first matching name. |
| -c, --cache | Use the compressed filename cache for listing or searching. |
| --cache release | Remove only the specified archive cache. |
| -- | Treat the remaining arguments as paths, including names starting with a dash. |

--find=TEXT, --first=TEXT and --cache=release are also accepted.
Do not combine MEMBER extraction with listing, search or cache options.
A search with no matching names exits with status 1; argument errors exit with
status 2. --help writes to standard output and exits successfully.

Examples:

```sh
# Show the full help.
tgzcat --help

# List every member of a tar.gz or tgz archive.
tgzcat I26282X.tar.gz
tgzcat observation.tgz

# Read a DRG text file without extracting it to disk (basename match).
tgzcat I26282X.tar.gz I26282X.DRG | less

# Extract a binary cor member to a file (full-path match).
tgzcat I26282X.tar.gz I26282X/x/cor/scan.cor > scan.cor

# List every filename containing this baseline name.
tgzcat I26282X.tar.gz --find YAMAGU32_YAMAGU34

# Print the first matching filename and stop early.
tgzcat I26282X.tar.gz --first .DRG

# Build or reuse a compressed list cache.
tgzcat I26282X.tar.gz --cache

# Search the cached list, building the complete list cache if necessary.
tgzcat I26282X.tar.gz --cache --find YAMAGU32_YAMAGU34

# The same search with short options or options first.
tgzcat -c -f YAMAGU32_YAMAGU34 I26282X.tar.gz

# Use grep when a regular expression, rather than a literal substring, is needed.
tgzcat I26282X.tar.gz --cache | grep "YAMAGU32_YAMAGU34.*[.]cor$"

# Remove only the cache belonging to this archive.
tgzcat I26282X.tar.gz --cache release

# Interpret a dash-prefixed filename as an archive, not an option.
tgzcat -- -observation.tar.gz
```

MEMBER outputs file contents; --find and --first output filenames.
The reader also handles concatenated gzip members and uncompressed .tar inputs.
Complete listing scans validate the remaining gzip stream, including its CRC.
Reaching a late member in gzip requires decompressing everything before it.
--first and MEMBER extraction stop early and do not validate the unread remainder.

Caching is opt-in and stores filenames only. --cache may be combined with
--find or --first. A --find cache build stores the complete list, including
names excluded by the filter. --first uses an existing cache; if none is valid,
it searches the archive and stops early without creating an incomplete cache.
Both the older options-first forms and archive-first forms remain supported.

All new caches are stored in one directory, in this order of precedence:

- TGZCAT_CACHE_DIR, if set
- $XDG_CACHE_HOME/tgzcat, if XDG_CACHE_HOME is an absolute path
- $HOME/.cache/tgzcat

Each archive has one <path-hash>.list.z file. Its canonical absolute path
determines the name, so archives with identical basenames remain separate,
symlink aliases share a cache, and replacing an archive overwrites its old cache.
The file contains a 64-byte metadata header followed by a zlib-compressed list.
Only filenames are stored. Device, inode, size, modification and change times
are checked before reuse. The compressed stream is verified before output;
a truncated or corrupt cache is rebuilt without duplicating the listing.
Compression/decompression uses bounded buffers.

Remove only the specified archive cache:

    tgzcat observation.tar.gz --cache release
    tgzcat --cache release observation.tar.gz

Release also removes abandoned temporary files for that archive. It preserves
other archives, unrelated files and the cache directory. Repeating release is
safe when the selected cache is absent. A bare tgzcat --cache release is rejected;
there is no command that clears all archive caches.
A recognized .tgzcat-list file from earlier versions alongside the specified
archive is also removed. Old sidecars are not automatically migrated or scanned
globally. New caches are published by atomic rename; interrupted builds may
leave a temporary file which release can remove.
If a cache cannot be written, ordinary listing still works.

Run behavior checks with make check (or make check BACKEND=zlib).
They cover extraction, GNU/PAX long names, buffer boundaries, concatenated
gzip members, checksum errors, plain tar inputs, filters and cache behavior.

To measure scanning and compressed-cache use (test caches stay in a temporary directory):

    make check ARCHIVE=/path/to/observation.tar.gz
    make check ARCHIVE=/path/to/observation.tar.gz BASELINE=/path/to/old-tgzcat

Timing uses the median of three runs and reports peak RSS with /usr/bin/time.
On I26282X.tar.gz (349 MiB, 733 names), the measured listing time changed from
1.55 s to 0.98 s, and late-member extraction from 1.72 s to 1.07 s.
Peak RSS changed from about 5.5 MiB to 2.9 MiB. Results depend on input and host.
For that archive, the filename list is 60,903 bytes; the compressed cache is
3,388 bytes including metadata (4,096 bytes allocated on the tested filesystem).
Cache hits took about 0.0011 s. Sizes depend on the filename list and filesystem.

Third-party source and licenses:
- https://github.com/rust-lang/flate2-rs (LICENSE.flate2-MIT / LICENSE.flate2-APACHE)
- https://github.com/trifectatechfoundation/zlib-rs (LICENSE.zlib-rs)
