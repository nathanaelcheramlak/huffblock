# Phase audit and validation

Audit date: 22 October 2026 (Africa/Nairobi). All three agreed implementation
phases are complete. The format remains version 1; the CLI is version 0.3.0.

## Phase 1: correct sequential compression

| Requirement | Implementation and evidence |
|---|---|
| Written binary format | [FORMAT.md](FORMAT.md): explicit little-endian integers, reserved fields, code lengths, meaningful bit counts, zero padding, CRC-32, and a mandatory end marker |
| Canonical Huffman codec | Frequency counts, deterministic tree ties, canonical assignment, empty-file framing, single-symbol codes, and a maximum code length of 32 |
| Raw fallback | Raw storage when the complete Huffman body does not save space, or when a tree requires codes longer than 32 bits |
| Independent block codec | Public block API has owned results and no file I/O or shared mutable working state |
| Sequential implementation | `--threads 1` runs on the calling thread; `HUFFBLOCK_REFERENCE_CODEC=ON` additionally retains the original per-bit paths |
| Correctness beyond encoder/decoder agreement | Independent CRC vectors, exact raw/empty archive bytes, independently packed canonical codes, a 32-bit all-ones code, and 129 short-tail/long-code boundary examples |
| Boundary and corruption handling | Empty, tiny, single-symbol, uniform, seeded random, mixed raw/Huffman, partial/exact/default/max-size blocks; malformed lengths, sizes, padding, checksums, truncation, and trailing data |
| File safety and CLI | Existing files and input aliases preserved; missing inputs, bad arguments, read/write/flush errors, Unicode names, spaces, and dash-prefixed paths covered |

The original 12 codec/archive groups remain in CTest. An additional group
independently checks unaligned 32-bit encoder codes and the maximum 39-bit writer
reservoir occupancy. See [tests.cpp](../tests/tests.cpp).

## Phase 2: bounded parallel processing

| Requirement | Implementation and evidence |
|---|---|
| Fixed worker pool | `std::jthread` workers; one reader; calling thread writes; each stream has one owner |
| Bounded memory | Credit reserved before reading/allocation and released after writing and destroying a result; queued, active, completed, and currently written blocks all count |
| Ordered output | Sequence numbers and a bounded ring; tests hold block zero while later blocks finish and verify that writing cannot overtake it |
| Determinism | Exact archive comparisons across 1/2/4/8 workers and admission limits 1/2/5/16, including fewer slots than workers |
| Parallel decoding | Independent block decode jobs verify CRC before their ordered write |
| Error propagation and cancellation | Injected reader, worker, writer, codec, and I/O errors; shared stop token; waiting threads wake and all workers join before errors return |
| Startup exception safety | Startup is inside the shutdown/error handler; a partially constructed pool is stopped and joined. This path was reviewed, not forced by exhausting OS threads |
| Temporary outputs | Complete closed files published with no-replace operations; early/late failure cleanup, late destination collisions, two concurrent publishers, and dangling symlinks tested |

All eight parallel/publication test groups pass. The slow-first-block test
checks actual admission counts while work is stalled, rather than relying on
timing guesses. See [parallel.cpp](../tests/parallel.cpp).

Cancellation is cooperative between bounded codec calls. It does not interrupt
an arbitrary blocking stream read/write. Completed-output publication is not a
power-loss durability guarantee; the payload and parent directory are not fsynced.

## Phase 3: hardening and measured performance

| Requirement | Implementation and evidence |
|---|---|
| Resource limits | Default 256 MiB live block-buffer budget and configurable decoded output maximum, checked before block body allocation |
| Arithmetic safety | Checked cumulative-size addition, including exact `UINT64_MAX` and overflow tests; bounded code lengths and shifts |
| Malformed-input automation | 15,796 deterministic mutation/truncation cases (30 valid mutations accepted, 15,766 rejected), plus 128 threaded checksum-failure cases; fixed seed `0x48554646` |
| Coverage-guided fuzzing | Clang 14 + libFuzzer + ASan/UBSan: 1,114,706 executions, 369 new units, no crash or sanitizer finding; 30-second budget, 31-second reported execution |
| Sanitizers | Entire four-suite CTest set passed under GCC 11.4 ASan/UBSan and TSan |
| Reproducible benchmarks | Seeded datasets, per-case warmup, repeated trials, exact restoration checks, archive hash checks, machine/build metadata, JSON/CSV, and wall throughput/ratio/peak RSS/scaling |
| Profiling and optimization | Separate gprof builds identified the codec as the sampled bottleneck; byte-oriented writing and a 10-bit decode lookup improved measured Huffman workloads without changing archive bytes |
| Platform verification | Linux GCC and native Windows LLVM-MinGW builds/tests; Windows/Linux encode/decode interoperability and exact archive comparisons |
| Documentation and CI | Usage, format, memory/cancellation tradeoffs, raw performance observations, reproducible commands, and Windows/Linux/sanitizer/reference/fuzzer CI configuration |

See [adversarial.cpp](../tests/adversarial.cpp), the
[fuzzer log](../benchmarks/results/fuzz-linux.txt), and
[the measured performance report](BENCHMARKS.md).

The final passing [ASan/UBSan test log](../benchmarks/results/asan-tests.txt) and
[TSan test log](../benchmarks/results/tsan-tests.txt) are also preserved.

The fuzzer used seed `1941211696`, three independently constructed format seeds,
maximum input length 16,384, RSS limit 512 MiB, and per-input timeout three seconds.
Its observed peak RSS was 311 MiB, including sanitizer/fuzzer overhead. This is a
bounded exploratory campaign, not a proof that every malformed input is safe.

## Locally executed checks

| Configuration | Result |
|---|---|
| Ubuntu 22.04 / GCC 11.4 / Release optimized | All 4 CTest suites passed |
| Ubuntu 22.04 / GCC 11.4 / Debug optimized | All 4 CTest suites passed |
| Ubuntu 22.04 / GCC 11.4 / Release reference codec | All 4 CTest suites passed |
| Ubuntu 22.04 / GCC 11.4 / Debug ASan + UBSan | All 4 CTest suites passed |
| Ubuntu 22.04 / GCC 11.4 / Debug TSan | All 4 CTest suites passed, no warnings |
| Native Windows / LLVM-MinGW Clang 23.1.4 / Release | All 4 CTest suites passed |
| Native Windows / LLVM-MinGW Clang 23.1.4 / Debug | All 4 CTest suites passed |
| Clang 14 / libFuzzer + ASan + UBSan | Campaign passed |
| Windows/Linux archives, 512-byte and 64 KiB blocks, 1 and 4 workers | Identical archives and successful cross-decoding in all 4 cases |

CTest comprises 13 codec/archive groups, eight parallel groups,
three hardening groups, and CLI integration checks. The CLI tests verify process
exit codes and file contents, including cleanup after resource-limit errors.

TSan initially could not start under this WSL kernel's randomized memory layout.
It passed using `setarch x86_64 -R ctest ...`, which applies only to that process
tree. No host-wide ASLR setting or sanitizer suppression was used. The test
harness also uses system-clock deadlines to avoid GCC 11's known timed-wait
interception issue, as documented in its source.

Memory measurement was independently checked: a Python launcher with an 82,396 KiB
high-water mark launched a CLI whose isolated peak was 3,512 KiB. Final reports
use the isolated GNU time measurement, not the contaminated direct Python
fork/exec high-water mark. Windows measurement uses `GetProcessMemoryInfo`.

Native MSVC and remote GitHub Actions were not executed locally. The
[workflow](../.github/workflows/ci.yml) includes MSVC Windows Debug/Release,
Linux Debug/Release, reference coding, both sanitizer modes, and libFuzzer for
future pushes. No remote repository, PR, or publishing action was performed.

## Scope and remaining limits

- Bytewise Huffman exploits byte frequencies, not repeated strings. Repetitive
  uniform-byte data, random data, and already compressed files can use raw blocks.
- Block framing still adds overhead to raw data. CRC-32 detects accidental damage
  and does not authenticate files.
- The output limit is unlimited unless selected. The buffer budget excludes
  stacks, codec scratch, standard-library buffers, and allocator retention.
- Temporary publication is available for file APIs. Caller-provided streams may
  contain partial output after an error. Abrupt termination may leave a temporary
  directory; normal error paths clean up owned temporary files.
- Performance observations are synthetic, warm-cache, shared-host measurements.
  More workers and larger blocks are not guaranteed to improve every workload.
- Random access/indexes, directory archives, encryption, LZ77/BWT, and additional
  compression formats were deliberately deferred beyond the three phases.
