#!/usr/bin/env python3
"""Behavior checks; optional real-archive timing: make check ARCHIVE=/path/file.tgz."""
import concurrent.futures
import gzip
import statistics
import io
import os
import zlib
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import time

BIN = str(Path(__file__).resolve().parent / "tgzcat")


def run(*args, check=True, env=None, cwd=None):
    return subprocess.run([BIN, *map(str, args)], capture_output=True, check=check, env=env, cwd=cwd)


def archive(path, entries, fmt=tarfile.PAX_FORMAT):
    with tarfile.open(path, "w:gz", format=fmt) as tar:
        for name, data in entries:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))


def checks(root):
    os.environ["TGZCAT_CACHE_DIR"] = str(root / "cache")
    path = root / "fixture.tar.gz"
    entries = [("dir/sample.DRG", bytes(range(256)) * 100),
               ("nested/" + "a" * 130 + ".cor", b"complex data"),
               ("dir/empty.cor", b"")]
    for fmt in [tarfile.PAX_FORMAT, tarfile.GNU_FORMAT]:
        archive(path, entries, fmt)
        expected = b"".join(name.encode() + b"\n" for name, _ in entries)
        assert run(path).stdout == expected
        cor_names = [name for name, _ in entries if ".cor" in name]
        assert run("--find", ".cor", path).stdout == b"".join(
            name.encode() + b"\n" for name in cor_names)
        assert run("--first", ".cor", path).stdout == cor_names[0].encode() + b"\n"
        missing = run("--find", "not-present", path, check=False)
        assert missing.stdout == b"" and missing.returncode == 1
        assert run("--first", "not-present", path, check=False).returncode == 1
        assert not Path(str(path) + ".tgzcat-list").exists()
        assert run("--cache", path).stdout == expected
        cached, = (root / "cache").glob("*.list.z")
        assert zlib.decompress(cached.read_bytes()[64:]) == expected
        stamp = cached.stat().st_mtime_ns
        assert run("--cache", path).stdout == expected
        assert cached.stat().st_mtime_ns == stamp, "cache was needlessly rebuilt"
        for name, data in entries:
            assert run(path, name).stdout == data
            assert run(path, Path(name).name).stdout == data
            assert run(path, name, "--cache").stdout == data
            assert run(path, Path(name).name, "--cache").stdout == data
        assert run(path, "missing", check=False).returncode == 1

        # A truncated cache must be rejected and rebuilt.
        cached.write_bytes(cached.read_bytes()[:-1])
        assert run("--cache", path).stdout == expected
        cached.unlink()

    # Changing the archive must invalidate a previously valid cache.
    run("--cache", path)
    archive(path, [("replacement.cor", b"new data")])
    assert run("--cache", path).stdout == b"replacement.cor\n"
    assert run(path, "replacement.cor").stdout == b"new data"

    # Concurrent builders/readers must all return the complete same list.
    cached.unlink()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        outputs = list(pool.map(lambda _: run("--cache", path).stdout, range(8)))
    assert all(data == b"replacement.cor\n" for data in outputs)
    assert not list(root.rglob("*.tmp.*"))

    # Failed scans must not publish a cache.
    bad = root / "bad.tar.gz"
    bad.write_bytes(path.read_bytes()[:20])
    assert run("--cache", bad, check=False).returncode != 0
    assert not Path(str(bad) + ".tgzcat-list").exists()
    assert not list(root.rglob("*.tmp.*"))

    # Cache creation failure falls back to an ordinary listing.
    uncached = root / "uncached.tar.gz"
    archive(uncached, [("fallback.cor", b"data")])
    blocked = root / "blocked"
    blocked.write_bytes(b"not a directory")
    blocked_env = dict(os.environ, TGZCAT_CACHE_DIR=str(blocked / "cache"))
    assert run("--cache", uncached, env=blocked_env).stdout == b"fallback.cor\n"
    assert not list(root.rglob("*.tmp.*"))
    # Payloads spanning the fixed decoder buffer, and concatenated gzip members.
    data = bytes(range(256)) * 11000
    large = root / "large.tar.gz"
    archive(large, [("large.cor", data), ("after.DRG", b"end")])
    assert run(large, "large.cor").stdout == data
    assert run(large, "after.DRG").stdout == b"end"
    raw = gzip.decompress(large.read_bytes())
    plain = root / "plain.tar"
    plain.write_bytes(raw)
    assert run(plain).stdout == b"large.cor\nafter.DRG\n"
    assert run(plain, "large.cor").stdout == data
    concat = root / "concatenated.tar.gz"
    cuts = [0, 17, 513, 5110, len(raw)]
    concat.write_bytes(b"".join(gzip.compress(raw[a:b])
                                for a, b in zip(cuts, cuts[1:])))
    assert run(concat).stdout == run(large).stdout
    assert run(concat, "large.cor").stdout == data
    corrupt = bytearray(large.read_bytes())
    corrupt[-8] ^= 1  # Damage gzip CRC, after the complete tar payload.
    crc = root / "bad-crc.tar.gz"
    crc.write_bytes(corrupt)
    assert run("--cache", crc, check=False).returncode != 0
    assert not Path(str(crc) + ".tgzcat-list").exists()
    short = root / "short-header.tar.gz"
    short.write_bytes(gzip.compress(b"x" * 127))
    assert run(short, check=False).returncode != 0
    assert not list(root.rglob("*.tmp.*"))
    print("All checks passed: extraction, filters, gzip members/CRC, plain tar, and cache behavior.")




def central_cache_checks(root):
    directory = root / "cache"
    # Same basenames in different directories must have independent caches.
    first = root / "fixture.tar.gz"
    second_dir = root / "second"
    second_dir.mkdir()
    second = second_dir / "fixture.tar.gz"
    archive(second, [("second.cor", b"other")])
    run("--cache", first)
    first_cache, = directory.glob("*.list.z")
    run(second, "--cache")
    second_cache, = set(directory.glob("*.list.z")) - {first_cache}
    first_index = Path(str(first_cache)[:-7] + ".index.gz")
    second_index = Path(str(second_cache)[:-7] + ".index.gz")
    assert first_index.exists() and second_index.exists()
    keep = directory / "unrelated.txt"
    keep.write_bytes(b"keep")
    first_temp = Path(str(first_cache) + ".tmp.abandoned")
    second_temp = Path(str(second_cache) + ".tmp.abandoned")
    first_temp.write_bytes(b"temporary")
    second_temp.write_bytes(b"temporary")
    legacy = Path(str(first) + ".tgzcat-list")
    legacy.write_bytes(b"TGZLST1\0old cache")
    run(first, "--cache", "release")
    assert not first_cache.exists() and not first_temp.exists() and not legacy.exists()
    assert not first_index.exists() and second_index.exists()
    assert second_cache.exists() and second_temp.exists() and keep.read_bytes() == b"keep"
    assert run("--cache", "release", check=False).returncode == 2
    assert second_cache.exists(), "bare release must not delete all caches"
    run("--cache", "release", second)
    assert not second_cache.exists() and not second_temp.exists() and keep.exists()
    assert not second_index.exists()
    run(second, "--cache", "release")  # Idempotent even when no cache exists.

    # Replacing the archive reuses its path, without leaving old versions behind.
    run("--cache", first)
    archive(first, [("changed.cor", b"changed")])
    assert run("--cache", first).stdout == b"changed.cor\n"
    assert set(directory.glob("*.list.z")) == {first_cache}
    alias = root / "alias.tar.gz"
    alias.symlink_to(first)
    stamp = first_cache.stat().st_mtime_ns
    assert run("--cache", alias).stdout == b"changed.cor\n"
    assert first_cache.stat().st_mtime_ns == stamp
    run(alias, "--cache", "release")
    assert not first_cache.exists()

    # Release by the original name still works after removal of the archive.
    run("--cache", second)
    second.unlink()
    run(second, "--cache", "release")
    assert not second_cache.exists()

    # Honor XDG and HOME without touching the real user cache.
    env = dict(os.environ)
    env.pop("TGZCAT_CACHE_DIR", None)
    env["XDG_CACHE_HOME"] = str(root / "xdg")
    run("--cache", first, env=env)
    assert len(list((root / "xdg" / "tgzcat").glob("*.list.z"))) == 1
    run(first, "--cache", "release", env=env)
    assert not list((root / "xdg" / "tgzcat").glob("*.list.z"))
    env.pop("XDG_CACHE_HOME")
    env["HOME"] = str(root / "home")
    run("--cache", first, env=env)
    assert len(list((root / "home" / ".cache" / "tgzcat").glob("*.list.z"))) == 1
    run(first, "--cache", "release", env=env)

    # Verify compression and rejection of a damaged compressed list.
    many = root / "many.tar.gz"
    names = [("experiment/cor/YAMAGU32_YAMAGU34_%06d_x.cor" % i, b"")
             for i in range(2000)]
    archive(many, names)
    expected = run(many).stdout
    assert run("--cache", many).stdout == expected
    compressed, = directory.glob("*.list.z")
    packed = compressed.read_bytes()
    assert zlib.decompress(packed[64:]) == expected
    assert len(packed) < len(expected) // 4
    damaged = bytearray(packed)
    damaged[-1] ^= 1
    compressed.write_bytes(damaged)
    assert run("--cache", many).stdout == expected, "corrupt cache duplicated output"
    assert zlib.decompress(compressed.read_bytes()[64:]) == expected
    run(many, "--cache", "release")
    assert not list(directory.glob("*.list.z")) and keep.exists()
    assert not list(root.rglob("*.tmp.*"))
    print("Central compressed cache checks passed; archive-specific release preserves other caches.")


def cli_checks(root):
    work = root / "cli"
    work.mkdir()
    path = work / "input.tar.gz"
    cache_dir = work / "cache"
    env = dict(os.environ, TGZCAT_CACHE_DIR=str(cache_dir))
    entries = [("dir/scan.cor", b"binary\0data"), ("other/scan.cor", b"second"),
               ("experiment.DRG", b"schedule"), ("--list", b"literal member")]
    archive(path, entries)
    expected = b"".join(name.encode() + b"\n" for name, _ in entries)
    matches = b"dir/scan.cor\nother/scan.cor\n"

    def command(*args, check=True, cwd=None):
        return run(*args, check=check, env=env, cwd=cwd)

    for flag in ["-h", "--help"]:
        help_result = command(flag)
        assert not help_result.stderr
        for text in [b"Usage:", b"Options", b"Examples:", b"--cache release", b"--first"]:
            assert text in help_result.stdout
    bare = command()
    assert bare.stdout == command("--help").stdout
    assert bare.stderr == b""

    for args in [(path,), ("--list", path), (path, "-l")]:
        assert command(*args).stdout == expected
    for args in [("--find", "scan", path), (path, "--find", "scan"),
                 ("-f", "scan", path), (path, "--find=scan")]:
        assert command(*args).stdout == matches
    for args in [("--first", "scan", path), (path, "--first", "scan"),
                 ("-1", "scan", path), (path, "--first=scan")]:
        assert command(*args).stdout == b"dir/scan.cor\n"
    assert command(path, "--find", "").stdout == expected
    assert command(path, "--first=").stdout == b"dir/scan.cor\n"
    assert command(path, "scan.cor").stdout == entries[0][1]
    assert command(path, "scan.cor", "--cache").stdout == entries[0][1]
    assert command("-c", path, "experiment.DRG").stdout == b"schedule"
    assert not list(cache_dir.glob("*.list.z")), "member extraction created a list cache"
    assert len(list(cache_dir.glob("*.index.gz"))) == 1
    assert command(path, "--", "--list").stdout == b"literal member"

    dashed = work / "-input.tar.gz"
    dashed.write_bytes(path.read_bytes())
    assert command("--", "-input.tar.gz", cwd=work).stdout == expected

    invalid = [("--unknown", path), ("--find",), ("--first",), ("--cache", "release"),
               (path, "scan.cor", "--list"),
               (path, "scan.cor", "--find", "scan"),
               (path, "--find", "scan", "--first", "scan"),
               (path, "--cache", "release", "--find", "scan"),
               (path, "one", "two"), ("--cache=invalid", path)]
    for args in invalid:
        result = command(*args, check=False)
        assert result.returncode == 2, args
        assert result.stdout == b"" and b"--help" in result.stderr, args

    # A filtered cache build must still cache every filename.
    assert command(path, "-c", "-f", "scan").stdout == matches
    cached, = cache_dir.glob("*.list.z")
    assert zlib.decompress(cached.read_bytes()[64:]) == expected
    stamp = cached.stat().st_mtime_ns
    assert command("--cache", "--first=scan", path).stdout == b"dir/scan.cor\n"
    assert command(path, "--find", "absent", "-c", check=False).returncode == 1
    assert command("-c", path).stdout == expected
    assert command(path, "scan.cor", "-c").stdout == entries[0][1]
    assert command("--cache", path, "experiment.DRG").stdout == b"schedule"
    assert command(path, "missing.cor", "--cache", check=False).returncode == 1
    assert cached.stat().st_mtime_ns == stamp, "search rebuilt a valid cache"
    command(path, "--cache=release")
    assert not cached.exists()
    assert command(path, "--first", "scan", "-c").stdout == b"dir/scan.cor\n"
    assert not list(cache_dir.glob("*.list.z")), "early stop published an incomplete cache"
    assert command("--find", "absent", path, "--cache", check=False).returncode == 1
    listed = command(path, "--cache", "-v")
    assert listed.stdout == expected
    assert b"list cache hit" in listed.stderr
    command("--cache", "release", path)

    # Cache filtering must handle names that span decompression buffers.
    long_path = "long/" + "a" * 17000 + "/needle.cor"
    archive(path, [(long_path, b""), ("last.cor", b"")])
    expected_long = (long_path + "\nlast.cor\n").encode()
    listed = command(path, "--cache", "-v")
    assert listed.stdout == expected_long
    assert b"building extraction index" in listed.stderr
    assert command(path, "--cache", "--find", "needle").stdout == (long_path + "\n").encode()
    assert command("-c", "-1", "last", path).stdout == b"last.cor\n"
    pipe = subprocess.Popen([BIN, str(path), "--cache", "--find", "needle"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    pipe.stdout.close()
    assert pipe.wait(timeout=5) == 0, "closed downstream pipe should exit cleanly"
    assert pipe.stderr.read() == b""
    pipe.stderr.close()
    command(path, "--cache", "release")
    print("CLI checks passed: help, argument order, aliases, errors, '--', and cached searches.")

def indexed_extraction_checks(root):
    import random
    import re
    import struct
    work = root / "index"
    work.mkdir()
    directory = work / "cache"
    env = dict(os.environ, TGZCAT_CACHE_DIR=str(directory))
    path = work / "large.tar.gz"
    pattern = random.Random(42).randbytes(12345)
    leading = (pattern * ((48 * 1024 * 1024) // len(pattern) + 1))[:48 * 1024 * 1024]
    tail = (pattern * 200)[:2 * 1024 * 1024]
    entries = [("leading.cor", leading), ("dir/late.DRG", b"late schedule\n"),
               ("dir/after.cor", tail), ("else/late.DRG", b"duplicate"), ("empty.cor", b"")]
    archive(path, entries)

    def command(*args, check=True):
        return run(*args, check=check, env=env)

    # An extraction cache miss emits one payload and finishes building the index.
    first = command(path, "late.DRG", "--cache", "-v")
    assert first.stdout == b"late schedule\n"
    assert b"building extraction index" in first.stderr
    index, = directory.glob("*.index.gz")
    assert not list(directory.glob("*.list.z"))
    stamp = index.stat().st_mtime_ns
    first_name = command(path, "--cache", "--first", "late.DRG", "-v")
    assert first_name.stdout == b"dir/late.DRG\n"
    assert b"listing from extraction index" in first_name.stderr
    assert not list(directory.glob("*.list.z"))
    for name, data in entries:
        result = command(path, name, "--cache", "-v")
        assert result.stdout == data, name
        assert b"index hit" in result.stderr, result.stderr
        assert index.stat().st_mtime_ns == stamp
    result = command(path, "late.DRG", "--cache", "-v")
    assert result.stdout == b"late schedule\n", "basename selected a later duplicate"
    position = re.search(rb"compressed offset=(\d+), uncompressed offset=(\d+), skip=(\d+)",
                         result.stderr)
    assert position and int(position[1]) > 0 and int(position[2]) >= 16 * 1024 * 1024
    missing = command(path, "absent.cor", "--cache", "-v", check=False)
    assert missing.returncode == 1 and missing.stdout == b""
    assert b"index hit; member is absent" in missing.stderr

    # Inspect the format to ensure this exercised dictionary and bit restoration.
    decoded = gzip.decompress(index.read_bytes())
    assert decoded[64:72] == b"TGZIDX1\0"
    offset = 73
    points = []
    while True:
        tag = decoded[offset]
        offset += 1
        if tag == 1:
            point = struct.unpack_from("=QQII", decoded, offset)
            offset += 24 + point[3]
            points.append(point)
        elif tag == 2:
            member = struct.unpack_from("=QQII", decoded, offset)
            offset += 24 + member[2]
        elif tag == 0:
            total, count, members = struct.unpack_from("=QQQ", decoded, offset)
            assert count == len(points) and members == len(entries)
            assert total > len(leading)
            break
        else:
            raise AssertionError(tag)
    assert len(points) >= 2 and any(point[3] == 32768 for point in points)
    assert any(point[2] != 0 for point in points), "non-byte-aligned checkpoints not exercised"

    # A corrupt index is rebuilt before any cached output is emitted.
    index.write_bytes(index.read_bytes()[:-4])
    result = command(path, "late.DRG", "--cache", "-v")
    assert result.stdout == b"late schedule\n" and b"building extraction index" in result.stderr
    assert command(path, "late.DRG", "--cache").stdout == b"late schedule\n"
    assert not list(work.rglob("*.tmp.*"))

    # Listing from an existing extraction index can create its separate small list cache.
    expected = b"".join(name.encode() + b"\n" for name, _ in entries)
    listed = command(path, "--cache", "-v")
    assert listed.stdout == expected
    assert b"listing from extraction index" in listed.stderr
    listing, = directory.glob("*.list.z")
    command(path, "--cache", "release")
    assert not index.exists() and not listing.exists()

    pipe = subprocess.Popen([BIN, str(path), "leading.cor", "--cache"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    pipe.stdout.close()
    assert pipe.wait(timeout=10) == 0
    assert pipe.stderr.read() == b""
    pipe.stderr.close()
    assert not list(directory.glob("*.index.gz"))
    assert not list(work.rglob("*.tmp.*"))

    # Nonzero restart points inside later gzip members, including extraction across members.
    raw = gzip.decompress(path.read_bytes())
    concat = work / "concat.tgz"
    cuts = [0, 16 * 1024 * 1024 + 37, 32 * 1024 * 1024 + 81, len(raw)]
    concat.write_bytes(b"".join(gzip.compress(raw[a:b]) for a, b in zip(cuts, cuts[1:])))
    assert command(concat, "--cache").stdout == expected
    assert command(concat, "leading.cor", "--cache").stdout == leading
    assert command(concat, "late.DRG", "--cache").stdout == b"late schedule\n"
    assert command(concat, "after.cor", "--cache").stdout == tail
    command(concat, "--cache", "release")

    # 64-bit tar offsets, without allocating or reading the sparse payload.
    sparse = work / "large-offset.tar"
    size = 5 * 1024 ** 3
    header = tarfile.TarInfo("sparse.cor")
    header.size = size
    end = tarfile.TarInfo("tail.DRG")
    end.size = 4
    with sparse.open("wb") as output:
        output.write(header.tobuf(format=tarfile.USTAR_FORMAT))
        output.seek(size, 1)
        output.write(end.tobuf(format=tarfile.USTAR_FORMAT))
        output.write(b"tail" + b"\0" * 508 + b"\0" * 1024)
    assert command(sparse, "--cache").stdout == b"sparse.cor\ntail.DRG\n"
    result = command(sparse, "tail.DRG", "--cache", "-v")
    assert result.stdout == b"tail" and b"index hit" in result.stderr
    assert str(size + 1024).encode() in result.stderr
    command(sparse, "--cache", "release")

    # Replacing the source invalidates positions and dictionaries.
    archive(path, [("late.DRG", b"replacement")])
    rebuilt = command(path, "late.DRG", "--cache", "-v")
    assert rebuilt.stdout == b"replacement" and b"building extraction index" in rebuilt.stderr
    reused = command(path, "late.DRG", "--cache", "-v")
    assert reused.stdout == b"replacement" and b"index hit" in reused.stderr
    command(path, "--cache", "release")

    # Index creation failure preserves ordinary extraction.
    blocked = work / "blocked"
    blocked.write_bytes(b"not a directory")
    fallback = dict(env, TGZCAT_CACHE_DIR=str(blocked / "cache"))
    assert run(path, "late.DRG", "--cache", env=fallback).stdout == b"replacement"
    assert not list(work.rglob("*.index.gz"))
    assert not list(work.rglob("*.tmp.*"))
    print("Indexed extraction checks passed: restart bits/dictionaries, duplicates, CRC, gzip members, and >4 GiB offsets.")

def indexed_benchmark(root, source):
    import struct
    directory = root / "cache"
    index, = directory.glob("*.index.gz")
    decoded = gzip.decompress(index.read_bytes())
    offset = 73
    members, points = [], 0
    while True:
        tag = decoded[offset]
        offset += 1
        if tag == 1:
            point = struct.unpack_from("=QQII", decoded, offset)
            offset += 24 + point[3]
            points += 1
        elif tag == 2:
            member = struct.unpack_from("=QQII", decoded, offset)
            offset += 24
            name = decoded[offset:offset + member[2]].decode()
            offset += member[2]
            if member[3] in (ord("0"), ord("7")):
                members.append((name, member[1]))
        else:
            assert tag == 0
            break
    chosen = [next((name for name, size in reversed(members) if name.endswith(".DRG")), None),
              next((name for name, size in reversed(members)
                    if name.endswith(".cor") and "YAMAGU32_YAMAGU34" in name
                    and 0 < size <= 10 * 1024 * 1024), None)]
    for name in chosen:
        if not name:
            continue
        expected = None
        for cached in [False, True]:
            times, rss = [], []
            for _ in range(3):
                args = [BIN, str(source), name]
                if cached:
                    args += ["--cache", "--verbose"]
                start = time.perf_counter()
                result = subprocess.run(["/usr/bin/time", "-f", "RSS:%M", *args],
                                        capture_output=True, check=True)
                times.append(time.perf_counter() - start)
                rss.append(int(result.stderr.decode().split("RSS:")[-1].strip()))
                if expected is None:
                    expected = result.stdout
                assert result.stdout == expected
            print("%s %s: median %.4fs, peak RSS %d KiB" %
                  ("indexed" if cached else "uncached", Path(name).name,
                   statistics.median(times), max(rss)))
            if cached:
                print(result.stderr.decode().splitlines()[0])
    listing, = directory.glob("*.list.z")
    print("Cache storage: list %d bytes + index %d bytes (%d checkpoints), total %d bytes" %
          (listing.stat().st_size, index.stat().st_size, points,
           listing.stat().st_size + index.stat().st_size))
def cache_benchmark(root, source):
    expected = run(source).stdout
    start = time.perf_counter()
    assert run("--cache", source).stdout == expected
    build = time.perf_counter() - start
    cache, = (root / "cache").glob("*.list.z")
    packed = cache.read_bytes()
    assert zlib.decompress(packed[64:]) == expected
    times = []
    for _ in range(3):
        start = time.perf_counter()
        assert run(source, "--cache").stdout == expected
        times.append(time.perf_counter() - start)
    print("Compressed cache: %d -> %d bytes (%.1f%%), disk allocation %d bytes" %
          (len(expected), len(packed), len(packed) * 100 / max(1, len(expected)),
           cache.stat().st_blocks * 512))
    print("Cache build %.3fs; cache hit median %.4fs" % (build, statistics.median(times)))
def benchmark(root, source):
    source = str(Path(source).resolve(strict=True))
    baseline = sys.argv[2] if len(sys.argv) > 2 else None
    # No list caches are used. Measure three runs for each binary.
    binaries = [("current", BIN)]
    if baseline:
        binaries.insert(0, ("baseline", baseline))
    expected = {}
    listing = run(source).stdout.decode().splitlines()
    member = next((name for name in reversed(listing) if name.endswith(".DRG")), None)
    if member is None:
        member = next((name for name in listing if name.endswith(".cor")), None)
    cases = [("listing", [source])]
    if member:
        cases.append(("member extraction", [source, member]))
    for name, args in cases:
        for label, binary in binaries:
            times, rss = [], []
            for _ in range(3):
                start = time.perf_counter()
                result = subprocess.run(
                    ["/usr/bin/time", "-f", "RSS:%M", binary, *args],
                    capture_output=True, check=True)
                times.append(time.perf_counter() - start)
                rss.append(int(result.stderr.decode().split("RSS:")[-1].strip()))
                if name in expected:
                    assert result.stdout == expected[name], name + " output changed"
                else:
                    expected[name] = result.stdout
            print("%s %s: median %.3fs, peak RSS %d KiB" %
                  (label, name, statistics.median(times), max(rss)))
    names = expected["listing"].decode().splitlines()
    selected = [name for name in names if "YAMAGU32_YAMAGU34" in name]
    if not selected:
        print("No YAMAGU32_YAMAGU34 entry; first-match timing skipped")
        return
    assert run("--find", "YAMAGU32_YAMAGU34", source).stdout == (
        "".join(name + "\n" for name in selected).encode())
    start = time.perf_counter()
    assert run("--first", "YAMAGU32_YAMAGU34", source).stdout == (
        selected[0] + "\n").encode()
    print("First match: %.4fs; all %d listing entries agree" %
          (time.perf_counter() - start, len(names)))
if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="tgzcat-test-") as directory:
        root = Path(directory)
        checks(root)
        central_cache_checks(root)
        cli_checks(root)
        indexed_extraction_checks(root)
        if len(sys.argv) > 1:
            benchmark(root, sys.argv[1])
            cache_benchmark(root, sys.argv[1])
            indexed_benchmark(root, sys.argv[1])
