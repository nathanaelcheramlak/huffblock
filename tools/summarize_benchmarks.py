#!/usr/bin/env python3
"""Create a readable comparison from two benchmark.py JSON reports."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--before", type=Path, required=True)
parser.add_argument("--after", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--larger-reports", type=Path, nargs="*", default=[])
args = parser.parse_args()
before = json.loads(args.before.read_text())
after = json.loads(args.after.read_text())
for field in ("datasets", "archive_sha256", "size_mib", "seed", "repeats", "threads", "block_sizes"):
    if before["metadata"][field] != after["metadata"][field]:
        raise ValueError(f"reports are not comparable: {field}")
metadata = after["metadata"]
build_settings = {line.split(":", 1)[0]: line.split("=", 1)[1]
                  for line in metadata.get("cmake", []) if "=" in line}
build_description = (f"Build: {build_settings.get('CMAKE_BUILD_TYPE', 'see raw metadata')}; "
                     f"compiler `{build_settings.get('CMAKE_CXX_COMPILER', 'see raw metadata')}`; "
                     f"release flags `{build_settings.get('CMAKE_CXX_FLAGS_RELEASE', 'see raw metadata')}`.")


def row(report, dataset, block, workers, operation):
    return next(value for value in report["summary"] if
                (value["dataset"], value["block_size"], value["threads"], value["operation"]) ==
                (dataset, block, workers, operation))


lines = [
    "# Measured performance", "",
    f"Measured on {metadata['cpu']} ({metadata['logical_cpus']} logical CPUs), {metadata['platform']}.",
    build_description, "",
    f"Each synthetic dataset is {metadata['size_mib']} MiB (the gzip file is slightly larger). "
    f"Every configuration has one warmup followed by {metadata['repeats']} timed compression/decompression trials. "
    "Tables use median wall time; JSON retains every observation and min/max/standard deviation. "
    "Peak memory is the largest child-process peak RSS among the timed repetitions.", "",
    "Timing includes GNU time launcher startup, CLI startup, reading, checksums, writing, close, and publication. Generation and "
    "verification hashes are excluded. The filesystem cache is warm, storage is Linux filesystem "
    "temporary storage, and writes are not fsynced. This is a shared WSL host with no CPU pinning or "
    "frequency control. The two release builds were measured sequentially; these are local observations, "
    "not universal performance guarantees. RSS is measured by an isolated GNU time child to avoid "
    "inheriting the Python launcher's memory high-water mark.", "",
    "Datasets: generated log-like text; an 80% zero-byte distribution; repeated 0..255 bytes; "
    "seeded pseudorandom bytes; and gzip-compressed pseudorandom bytes. Text and skewed data repeat "
    "deterministic tiles. These are synthetic examples, not a substitute for a real-world corpus.", "",
    "## Effect of codec changes", "",
    "Reference and optimized builds share the format, checksums, resource limits, and pipeline. "
    "The reference uses the retained per-bit writer/tree decoder (`HUFFBLOCK_REFERENCE_CODEC=ON`). "
    "The optimized build uses a 64-bit write reservoir and a 10-bit decode table with tree fallback.", "",
    "All input SHA-256 values and compressed SHA-256 values match across the two reports and all worker counts.", "",
    "Single-thread results, 2 MiB blocks; throughput in MiB/s. Factor is optimized/reference.", "",
    "| Dataset | Encode reference | Encode optimized | Factor | Decode reference | Decode optimized | Factor |",
    "|---|---:|---:|---:|---:|---:|---:|",
]
block = 2097152
for dataset in metadata["datasets"]:
    old_c = row(before, dataset, block, 1, "compress")["mib_per_second"]
    new_c = row(after, dataset, block, 1, "compress")["mib_per_second"]
    old_d = row(before, dataset, block, 1, "decompress")["mib_per_second"]
    new_d = row(after, dataset, block, 1, "decompress")["mib_per_second"]
    lines.append(f"| {dataset} | {old_c:.1f} | {new_c:.1f} | {new_c/old_c:.2f}x | {old_d:.1f} | {new_d:.1f} | {new_d/old_d:.2f}x |")
lines += ["", "## Worker scaling and memory", "",
          "Optimized build, 2 MiB blocks. Ratio is archive bytes / original bytes; lower is smaller. "
          "Speedup compares against this same optimized build with one worker.", "",
          "| Dataset | Workers | Ratio | Encode MiB/s | Speedup | Encode RSS MiB | Decode MiB/s | Speedup | Decode RSS MiB |",
          "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
for dataset in metadata["datasets"]:
    for workers in metadata["threads"]:
        c = row(after, dataset, block, workers, "compress")
        d = row(after, dataset, block, workers, "decompress")
        lines.append(f"| {dataset} | {workers} | {c['ratio']:.4f} | {c['mib_per_second']:.1f} | "
                     f"{c['speedup']:.2f}x | {c['peak_rss_bytes']/1048576:.1f} | {d['mib_per_second']:.1f} | "
                     f"{d['speedup']:.2f}x | {d['peak_rss_bytes']/1048576:.1f} |")
lines += ["", "## Block-size comparison", "", "Optimized build, four workers.", "",
          "| Dataset | Block KiB | Ratio | Encode MiB/s | Decode MiB/s |",
          "|---|---:|---:|---:|---:|"]
for dataset in metadata["datasets"]:
    for size in metadata["block_sizes"]:
        c = row(after, dataset, size, 4, "compress")
        d = row(after, dataset, size, 4, "decompress")
        lines.append(f"| {dataset} | {size/1024:.0f} | {c['ratio']:.4f} | {c['mib_per_second']:.1f} | {d['mib_per_second']:.1f} |")
lines += ["", "## Interpretation", "",
          "The changes improve the measured text and skewed Huffman workloads. Raw blocks bypass "
          "the new bit writer and decode table; their observed changes include regressions as well "
          "as improvements. These results do not justify claiming that every input became faster.", "",
          "The 2 MiB default is retained as the original configuration with lower per-block framing "
          "overhead. The measured 256 KiB option offers useful throughput and memory tradeoffs, "
          "especially when a medium-sized file provides few large blocks. A universal best default "
          "would require a broader real-file corpus; block size remains configurable."]
if args.larger_reports:
    lines += ["", "## Larger inputs and memory", "",
              "Additional optimized text runs use the same warmup/repetition method. Each row "
              "is independent; the maximum RSS across repetitions is shown. Live buffer limits "
              "are enforced by admission accounting, not by these noisy RSS observations.", "",
              "| Input MiB | Workers | Block KiB | Encode MiB/s | Encode RSS MiB | Decode MiB/s | Decode RSS MiB |",
              "|---|---:|---:|---:|---:|---:|---:|"]
    reports = [after] + [json.loads(path.read_text()) for path in args.larger_reports]
    for report in reports:
        for workers in [1, 4]:
            for size in report["metadata"]["block_sizes"]:
                if report is after and size != 2097152:
                    continue
                c = row(report, "text", size, workers, "compress")
                d = row(report, "text", size, workers, "decompress")
                lines.append(f"| {report['metadata']['size_mib']} | {workers} | {size/1024:.0f} | "
                             f"{c['mib_per_second']:.1f} | {c['peak_rss_bytes']/1048576:.2f} | "
                             f"{d['mib_per_second']:.1f} | {d['peak_rss_bytes']/1048576:.2f} |")
    lines += ["", "With four workers and 2 MiB blocks, increasing text input from 64 to 128 MiB "
              "keeps measured encoding memory near 25 MiB and decoding near 32–34 MiB. This is "
              "consistent with bounded admission rather than retaining the entire file. Allocator "
              "retention and scheduling can change RSS even at fixed settings.", "",
              "Raw larger-input reports: [64 MiB](../benchmarks/results/linux-size64.json), "
              "[128 MiB](../benchmarks/results/linux-size128.json)."]
lines += ["", "## Reproduce", "", "```sh",
          "cmake -S . -B build-reference -DCMAKE_BUILD_TYPE=Release -DHUFFBLOCK_REFERENCE_CODEC=ON",
          "cmake --build build-reference --parallel 2",
          "cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release",
          "cmake --build build-release --parallel 2",
          "python3 tools/benchmark.py --exe build-reference/huffblock --output before.json --work-dir /var/tmp/huffblock-bench",
          "python3 tools/benchmark.py --exe build-release/huffblock --output after.json --work-dir /var/tmp/huffblock-bench",
          "python3 tools/benchmark.py --exe build-release/huffblock --output size64.json --work-dir /var/tmp/huffblock-bench --size-mib 64 --threads 1,4 --block-sizes 262144,2097152 --datasets text",
          "python3 tools/benchmark.py --exe build-release/huffblock --output size128.json --work-dir /var/tmp/huffblock-bench --size-mib 128 --threads 1,4 --block-sizes 2097152 --datasets text",
          "python3 tools/summarize_benchmarks.py --before before.json --after after.json --larger-reports size64.json size128.json --output comparison.md",
          "```", "", "Run the two measurements sequentially without other builds or profiling in progress. "
          "The benchmark aborts on a round-trip mismatch or any difference in compressed bytes between worker counts.", "",
          "Raw reports: [reference JSON](../benchmarks/results/linux-before.json), "
          "[optimized JSON](../benchmarks/results/linux-after.json), and matching CSV files in the same directory.", "",
          "Profiling reports: [before encoding](../benchmarks/results/profile-before-compress.txt), "
          "[before decoding](../benchmarks/results/profile-before-decompress.txt), "
          "[after encoding](../benchmarks/results/profile-after-compress.txt), "
          "[after decoding](../benchmarks/results/profile-after-decompress.txt). "
          "GNU gprof used a separate instrumented single-thread build and a 64 MiB text input. "
          "Its coarse samples identify the codec as the CPU bottleneck; the uninstrumented "
          "release comparisons above determine the effect of the changes.", ""]
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open("x", encoding="utf-8") as output:
    output.write("\n".join(lines))
print(args.output)
