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


def run(*args, check=True, env=None):
    return subprocess.run([BIN, *map(str, args)], capture_output=True, check=check, env=env)


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
    assert second_cache.exists() and second_temp.exists() and keep.read_bytes() == b"keep"
    assert run("--cache", "release", check=False).returncode == 2
    assert second_cache.exists(), "bare release must not delete all caches"
    run("--cache", "release", second)
    assert not second_cache.exists() and not second_temp.exists() and keep.exists()
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
        if len(sys.argv) > 1:
            benchmark(root, sys.argv[1])
            cache_benchmark(root, sys.argv[1])
