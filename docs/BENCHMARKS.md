# Measured performance

Measured on AMD Ryzen 7 8845HS w/ Radeon 780M Graphics (16 logical CPUs), Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.35.
Build: Release; compiler `/usr/bin/c++`; release flags `-O3 -DNDEBUG`.

Each synthetic dataset is 16 MiB (the gzip file is slightly larger). Every configuration has one warmup followed by 3 timed compression/decompression trials. Tables use median wall time; JSON retains every observation and min/max/standard deviation. Peak memory is the largest child-process peak RSS among the timed repetitions.

Timing includes GNU time launcher startup, CLI startup, reading, checksums, writing, close, and publication. Generation and verification hashes are excluded. The filesystem cache is warm, storage is Linux filesystem temporary storage, and writes are not fsynced. This is a shared WSL host with no CPU pinning or frequency control. The two release builds were measured sequentially; these are local observations, not universal performance guarantees. RSS is measured by an isolated GNU time child to avoid inheriting the Python launcher's memory high-water mark.

Datasets: generated log-like text; an 80% zero-byte distribution; repeated 0..255 bytes; seeded pseudorandom bytes; and gzip-compressed pseudorandom bytes. Text and skewed data repeat deterministic tiles. These are synthetic examples, not a substitute for a real-world corpus.

## Effect of codec changes

Reference and optimized builds share the format, checksums, resource limits, and pipeline. The reference uses the retained per-bit writer/tree decoder (`HUFFBLOCK_REFERENCE_CODEC=ON`). The optimized build uses a 64-bit write reservoir and a 10-bit decode table with tree fallback.

All input SHA-256 values and compressed SHA-256 values match across the two reports and all worker counts.

Single-thread results, 2 MiB blocks; throughput in MiB/s. Factor is optimized/reference.

| Dataset | Encode reference | Encode optimized | Factor | Decode reference | Decode optimized | Factor |
|---|---:|---:|---:|---:|---:|---:|
| text | 143.6 | 214.1 | 1.49x | 101.5 | 155.1 | 1.53x |
| skewed | 140.3 | 157.1 | 1.12x | 119.9 | 144.8 | 1.21x |
| repetitive | 217.7 | 200.9 | 0.92x | 250.0 | 244.8 | 0.98x |
| random | 225.9 | 212.4 | 0.94x | 249.4 | 263.5 | 1.06x |
| compressed | 216.4 | 224.4 | 1.04x | 246.5 | 263.3 | 1.07x |

## Worker scaling and memory

Optimized build, 2 MiB blocks. Ratio is archive bytes / original bytes; lower is smaller. Speedup compares against this same optimized build with one worker.

| Dataset | Workers | Ratio | Encode MiB/s | Speedup | Encode RSS MiB | Decode MiB/s | Speedup | Decode RSS MiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| text | 1 | 0.5831 | 214.1 | 1.00x | 6.9 | 155.1 | 1.00x | 7.0 |
| text | 2 | 0.5831 | 354.2 | 1.65x | 14.1 | 261.5 | 1.69x | 16.4 |
| text | 4 | 0.5831 | 549.9 | 2.57x | 22.4 | 439.6 | 2.83x | 20.8 |
| text | 8 | 0.5831 | 607.1 | 2.84x | 24.4 | 547.5 | 3.53x | 28.7 |
| skewed | 1 | 0.3221 | 157.1 | 1.00x | 6.3 | 144.8 | 1.00x | 6.4 |
| skewed | 2 | 0.3221 | 254.3 | 1.62x | 13.1 | 249.2 | 1.72x | 14.3 |
| skewed | 4 | 0.3221 | 435.2 | 2.77x | 21.2 | 418.0 | 2.89x | 16.6 |
| skewed | 8 | 0.3221 | 526.7 | 3.35x | 22.8 | 571.6 | 3.95x | 24.7 |
| repetitive | 1 | 1.0000 | 200.9 | 1.00x | 7.6 | 244.8 | 1.00x | 7.7 |
| repetitive | 2 | 1.0000 | 449.8 | 2.24x | 15.8 | 471.3 | 1.92x | 19.8 |
| repetitive | 4 | 1.0000 | 724.3 | 3.61x | 23.1 | 668.1 | 2.73x | 22.8 |
| repetitive | 8 | 1.0000 | 652.1 | 3.25x | 23.1 | 683.6 | 2.79x | 23.4 |
| random | 1 | 1.0000 | 212.4 | 1.00x | 7.6 | 263.5 | 1.00x | 7.6 |
| random | 2 | 1.0000 | 426.1 | 2.01x | 15.7 | 477.9 | 1.81x | 19.8 |
| random | 4 | 1.0000 | 603.9 | 2.84x | 21.8 | 667.1 | 2.53x | 24.2 |
| random | 8 | 1.0000 | 580.5 | 2.73x | 22.5 | 687.3 | 2.61x | 24.5 |
| compressed | 1 | 1.0000 | 224.4 | 1.00x | 7.7 | 263.3 | 1.00x | 7.7 |
| compressed | 2 | 1.0000 | 448.5 | 2.00x | 15.7 | 515.0 | 1.96x | 19.8 |
| compressed | 4 | 1.0000 | 727.1 | 3.24x | 23.2 | 790.6 | 3.00x | 23.9 |
| compressed | 8 | 1.0000 | 598.5 | 2.67x | 22.0 | 657.1 | 2.50x | 23.0 |

## Block-size comparison

Optimized build, four workers.

| Dataset | Block KiB | Ratio | Encode MiB/s | Decode MiB/s |
|---|---:|---:|---:|---:|
| text | 256 | 0.5840 | 669.0 | 525.6 |
| text | 2048 | 0.5831 | 549.9 | 439.6 |
| skewed | 256 | 0.3230 | 516.3 | 490.9 |
| skewed | 2048 | 0.3221 | 435.2 | 418.0 |
| repetitive | 256 | 1.0001 | 908.8 | 948.7 |
| repetitive | 2048 | 1.0000 | 724.3 | 668.1 |
| random | 256 | 1.0001 | 850.1 | 950.3 |
| random | 2048 | 1.0000 | 603.9 | 667.1 |
| compressed | 256 | 1.0001 | 857.7 | 929.7 |
| compressed | 2048 | 1.0000 | 727.1 | 790.6 |

## Interpretation

The changes improve the measured text and skewed Huffman workloads. Raw blocks bypass the new bit writer and decode table; their observed changes include regressions as well as improvements. These results do not justify claiming that every input became faster.

The 2 MiB default is retained as the original configuration with lower per-block framing overhead. The measured 256 KiB option offers useful throughput and memory tradeoffs, especially when a medium-sized file provides few large blocks. A universal best default would require a broader real-file corpus; block size remains configurable.

## Larger inputs and memory

Additional optimized text runs use the same warmup/repetition method. Each row is independent; the maximum RSS across repetitions is shown. Live buffer limits are enforced by admission accounting, not by these noisy RSS observations.

| Input MiB | Workers | Block KiB | Encode MiB/s | Encode RSS MiB | Decode MiB/s | Decode RSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| 16 | 1 | 2048 | 214.1 | 6.92 | 155.1 | 6.97 |
| 16 | 4 | 2048 | 549.9 | 22.39 | 439.6 | 20.77 |
| 64 | 1 | 256 | 219.8 | 4.08 | 159.4 | 4.00 |
| 64 | 1 | 2048 | 251.2 | 6.96 | 173.0 | 6.85 |
| 64 | 4 | 256 | 866.2 | 6.86 | 622.7 | 7.49 |
| 64 | 4 | 2048 | 793.8 | 24.69 | 582.7 | 31.53 |
| 128 | 1 | 2048 | 251.4 | 6.89 | 164.0 | 6.85 |
| 128 | 4 | 2048 | 919.6 | 24.68 | 630.9 | 33.73 |

With four workers and 2 MiB blocks, increasing text input from 64 to 128 MiB keeps measured encoding memory near 25 MiB and decoding near 32–34 MiB. This is consistent with bounded admission rather than retaining the entire file. Allocator retention and scheduling can change RSS even at fixed settings.

Raw larger-input reports: [64 MiB](../benchmarks/results/linux-size64.json), [128 MiB](../benchmarks/results/linux-size128.json).

## Reproduce

```sh
cmake -S . -B build-reference -DCMAKE_BUILD_TYPE=Release -DHUFFBLOCK_REFERENCE_CODEC=ON
cmake --build build-reference --parallel 2
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
python3 tools/benchmark.py --exe build-reference/huffblock --output before.json --work-dir /var/tmp/huffblock-bench
python3 tools/benchmark.py --exe build-release/huffblock --output after.json --work-dir /var/tmp/huffblock-bench
python3 tools/benchmark.py --exe build-release/huffblock --output size64.json --work-dir /var/tmp/huffblock-bench --size-mib 64 --threads 1,4 --block-sizes 262144,2097152 --datasets text
python3 tools/benchmark.py --exe build-release/huffblock --output size128.json --work-dir /var/tmp/huffblock-bench --size-mib 128 --threads 1,4 --block-sizes 2097152 --datasets text
python3 tools/summarize_benchmarks.py --before before.json --after after.json --larger-reports size64.json size128.json --output comparison.md
```

Run the two measurements sequentially without other builds or profiling in progress. The benchmark aborts on a round-trip mismatch or any difference in compressed bytes between worker counts.

Raw reports: [reference JSON](../benchmarks/results/linux-before.json), [optimized JSON](../benchmarks/results/linux-after.json), and matching CSV files in the same directory.

Profiling reports: [before encoding](../benchmarks/results/profile-before-compress.txt), [before decoding](../benchmarks/results/profile-before-decompress.txt), [after encoding](../benchmarks/results/profile-after-compress.txt), [after decoding](../benchmarks/results/profile-after-decompress.txt). GNU gprof used a separate instrumented single-thread build and a 64 MiB text input. Its coarse samples identify the codec as the CPU bottleneck; the uninstrumented release comparisons above determine the effect of the changes.
