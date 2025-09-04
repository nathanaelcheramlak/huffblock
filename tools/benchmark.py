#!/usr/bin/env python3
"""Reproducible CLI benchmarks; Python 3.9+ stdlib, plus GNU time on Linux."""

import argparse
import csv
import ctypes
import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


MIB = 1024 * 1024


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(MIB), b""):
            result.update(block)
    return result.hexdigest()


def generate(kind, size, seed):
    rng = random.Random(seed)
    if kind == "random":
        return rng.randbytes(size)
    if kind == "compressed":
        return gzip.compress(rng.randbytes(size), mtime=0)
    if kind == "text":
        tile = b"".join(
            f"2026-10-22 service=request worker={i % 16} status=200 bytes={i * 17} path=/api/items/{i}\n".encode()
            for i in range(1024)
        )
    elif kind == "skewed":
        tile = bytes(0 if rng.randrange(10) < 8 else rng.randrange(256) for _ in range(65536))
    elif kind == "repetitive":
        tile = bytes(range(256))
    else:
        raise ValueError(f"unknown dataset: {kind}")
    return (tile * ((size + len(tile) - 1) // len(tile)))[:size]


def windows_peak_rss(process):
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in (
                "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                "PagefileUsage", "PeakPagefileUsage"
            )
        ]

    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    query = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
    query.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    query.restype = wintypes.BOOL
    if not query(wintypes.HANDLE(int(process._handle)), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return counters.PeakWorkingSetSize


def measure(command):
    # A direct Python fork/exec + wait4 can inherit the launcher's resident-set
    # high-water mark. GNU time execs first, then forks from its small native
    # process, so its child's RSS measures the CLI rather than Python's corpus.
    with tempfile.TemporaryFile() as errors, tempfile.TemporaryDirectory(prefix="huffblock-measure-") as directory:
        stats = Path(directory) / "rss.txt"
        if sys.platform.startswith("linux"):
            time_executable = shutil.which("time")
            if not time_executable:
                raise RuntimeError("Linux benchmarks require GNU time (/usr/bin/time)")
            measured_command = [time_executable, "-q", "-f", "%M", "-o", str(stats), *command]
        elif os.name == "nt":
            measured_command = command
        else:
            raise RuntimeError("peak RSS collection currently supports Linux and Windows")
        start = time.perf_counter()
        process = subprocess.Popen(measured_command, stdout=subprocess.DEVNULL, stderr=errors)
        if hasattr(os, "wait4"):
            _, status, usage = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
            peak = int(stats.read_text().strip()) * 1024 if process.returncode == 0 else None
            cpu = usage.ru_utime + usage.ru_stime
        else:
            process.wait(timeout=120)
            peak = windows_peak_rss(process) if os.name == "nt" else None
            cpu = None
        elapsed = time.perf_counter() - start
        if process.returncode:
            errors.seek(0)
            raise RuntimeError(f"command failed ({process.returncode}): {command}\n" +
                               errors.read().decode(errors="replace"))
        return {"seconds": elapsed, "cpu_seconds": cpu, "peak_rss_bytes": peak}


def cpu_name():
    if Path("/proc/cpuinfo").is_file():
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return platform.processor()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new JSON report (never overwritten)")
    parser.add_argument("--work-dir", type=Path, default=Path("build-benchmark-data"))
    parser.add_argument("--size-mib", type=int, default=16)
    parser.add_argument("--threads", default="1,2,4,8")
    parser.add_argument("--block-sizes", default="262144,2097152")
    parser.add_argument("--datasets", default="text,skewed,repetitive,random,compressed")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20261022)
    args = parser.parse_args()
    threads = [int(value) for value in args.threads.split(",")]
    blocks = [int(value) for value in args.block_sizes.split(",")]
    kinds = args.datasets.split(",")
    if (args.size_mib < 1 or args.repeats < 1 or 1 not in threads or
            any(value < 1 or value > 256 for value in threads) or
            any(value < 1 or value > 16777216 for value in blocks)):
        parser.error("positive sizes/repeats and valid blocks/workers including --threads 1 are required")
    if args.output.exists() or args.output.with_suffix(".csv").exists():
        parser.error("JSON or CSV output already exists")
    executable = args.exe.resolve(strict=True)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    metadata = {
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(), "cpu": cpu_name(), "logical_cpus": os.cpu_count(),
        "python": platform.python_version(), "executable": str(executable),
        "executable_sha256": digest(executable),
        "version": subprocess.check_output([str(executable), "--version"], text=True).strip(),
        "size_mib": args.size_mib, "seed": args.seed, "repeats": args.repeats,
        "warmups_per_case": 1, "threads": threads, "block_sizes": blocks,
        "scope": "wall time includes process startup, file I/O, checksums, close and publication; excludes hashing/generation",
        "cache": "warm filesystem cache; no fsync, cache eviction, CPU pinning, or isolated host",
        "memory": "isolated GNU time child peak RSS on Linux; Windows peak working set via GetProcessMemoryInfo",
        "measurement_overhead": "Linux wall time includes GNU time launcher startup; CPU time includes helper and CLI",
        "synthetic": "text/skewed/repetitive repeat deterministic tiles; random uses full seeded bytes; compressed is gzip of seeded random",
        "work_filesystem_path": str(args.work_dir.resolve()),
    }
    metadata["version_process_peak_rss_bytes"] = measure([str(executable), "--version"])["peak_rss_bytes"]
    cache = executable.parent / "CMakeCache.txt"
    if cache.exists():
        metadata["cmake"] = [line for line in cache.read_text().splitlines() if line.startswith((
            "CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS:", "CMAKE_CXX_FLAGS_RELEASE:",
            "HUFFBLOCK_PROFILE:", "HUFFBLOCK_REFERENCE_CODEC:", "HUFFBLOCK_SANITIZER:"))]
    trials = []
    summaries = []
    archive_hashes = {}
    with tempfile.TemporaryDirectory(prefix="huffblock-benchmark-", dir=args.work_dir.resolve()) as directory:
        work = Path(directory)
        inputs = {}
        for kind in kinds:
            path = work / f"{kind}.bin"
            path.write_bytes(generate(kind, args.size_mib * MIB, args.seed))
            inputs[kind] = {"path": path, "bytes": path.stat().st_size, "sha256": digest(path)}
        metadata["datasets"] = {kind: {key: value for key, value in data.items() if key != "path"}
                                for kind, data in inputs.items()}
        cases = [(kind, block, count) for kind in kinds for block in blocks for count in threads]
        random.Random(args.seed).shuffle(cases)
        for kind, block, count in cases:
            original = inputs[kind]
            encoded = work / "output.hfb"
            restored = work / "restored.bin"
            samples = {"compress": [], "decompress": []}
            for repeat in range(-1, args.repeats):
                compress_command = [str(executable), "compress", "--threads", str(count),
                                    "--block-size", str(block), str(original["path"]), str(encoded)]
                decode_command = [str(executable), "decompress", "--threads", str(count),
                                  str(encoded), str(restored)]
                compression = measure(compress_command)
                decompression = measure(decode_command)
                if digest(restored) != original["sha256"]:
                    raise RuntimeError("benchmark round trip changed bytes")
                archive_hash = digest(encoded)
                key = (kind, block)
                if archive_hashes.setdefault(key, archive_hash) != archive_hash:
                    raise RuntimeError("worker count changed compressed output")
                stored = encoded.stat().st_size
                if repeat >= 0:
                    for operation, sample in (("compress", compression), ("decompress", decompression)):
                        trials.append(dict(dataset=kind, block_size=block, threads=count, repeat=repeat,
                                           operation=operation, original_bytes=original["bytes"],
                                           stored_bytes=stored, **sample))
                        samples[operation].append(sample)
                encoded.unlink()
                restored.unlink()
            for operation, values in samples.items():
                times = [value["seconds"] for value in values]
                peaks = [value["peak_rss_bytes"] for value in values if value["peak_rss_bytes"] is not None]
                median = statistics.median(times)
                summaries.append(dict(dataset=kind, block_size=block, threads=count, operation=operation,
                    original_bytes=original["bytes"], stored_bytes=stored, ratio=stored / original["bytes"],
                    median_seconds=median, min_seconds=min(times), max_seconds=max(times),
                    stdev_seconds=statistics.stdev(times) if len(times) > 1 else 0,
                    mib_per_second=original["bytes"] / MIB / median,
                    peak_rss_bytes=max(peaks) if peaks else None))
            print(f"{kind:10} block={block:8} workers={count:2}: "
                  f"encode={original['bytes'] / MIB / statistics.median(v['seconds'] for v in samples['compress']):7.1f} "
                  f"decode={original['bytes'] / MIB / statistics.median(v['seconds'] for v in samples['decompress']):7.1f} MiB/s",
                  flush=True)
    for row in summaries:
        baseline = next(item for item in summaries if item["dataset"] == row["dataset"] and
                        item["block_size"] == row["block_size"] and item["operation"] == row["operation"] and
                        item["threads"] == 1)
        row["speedup"] = baseline["median_seconds"] / row["median_seconds"]
    metadata["archive_sha256"] = {f"{kind}:{block}": value
                                   for (kind, block), value in archive_hashes.items()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as report:
        json.dump({"metadata": metadata, "summary": summaries, "trials": trials}, report, indent=2)
        report.write("\n")
    csv_path = args.output.with_suffix(".csv")
    with csv_path.open("x", newline="", encoding="utf-8") as report:
        writer = csv.DictWriter(report, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)
    print(f"Reports: {args.output} and {csv_path}")


if __name__ == "__main__":
    main()
