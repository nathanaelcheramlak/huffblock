# huffblock

A C++20, single-file, lossless compressor built to demonstrate systems engineering.
The project includes a sequential reference, bounded parallel processing,
resource limits, malformed-input testing, and reproducible performance tools.
Each block independently uses canonical Huffman coding or raw storage, with a
CRC-32 checksum of its original bytes.

## Build and test

Requires CMake 3.20 or newer and a C++20 compiler. No external libraries or test
frameworks are required.

Verified with GCC 11.4 on Ubuntu 22.04 under WSL and with native Windows
LLVM-MinGW/Clang 23.1.4. See [the phase audit and validation record](docs/VALIDATION.md)
for configurations, sanitizer/fuzzer results, and remaining limitations.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For single-configuration generators, the executable is `build/huffblock` on Linux
or `build/huffblock.exe` on Windows. With Visual Studio's default generator, use
`build/Release/huffblock.exe`. Run these commands in a Visual Studio developer
shell if using MSVC.

From PowerShell with an existing Ubuntu WSL toolchain, for this checkout:

```powershell
wsl -d Ubuntu-22.04 -- cmake -S /mnt/d/Dev/huffblock -B /mnt/d/Dev/huffblock/build-debug -DCMAKE_BUILD_TYPE=Debug
wsl -d Ubuntu-22.04 -- cmake --build /mnt/d/Dev/huffblock/build-debug --parallel 2
wsl -d Ubuntu-22.04 -- ctest --test-dir /mnt/d/Dev/huffblock/build-debug --output-on-failure
```

## Usage

```sh
./build/huffblock compress input.txt output.hfb
./build/huffblock decompress output.hfb restored.txt
./build/huffblock compress --block-size 65536 input.txt output.hfb
./build/huffblock compress --threads 4 --max-in-flight 8 input.txt parallel.hfb
./build/huffblock decompress --threads 4 parallel.hfb restored-parallel.txt
./build/huffblock compress --threads 1 input.txt sequential.hfb
./build/huffblock decompress --max-output 1073741824 --buffer-limit 67108864 output.hfb limited.txt
./build/huffblock --help
./build/huffblock --version
```

The default block size is 2 MiB (2,097,152 bytes). `--block-size` accepts decimal
byte counts from 1 through 16,777,216 and is valid only for compression. Use `--`
before paths beginning with `-`; quote paths containing spaces.

`--threads` accepts 1 through 256 for either command. By default, the worker
count is the detected hardware concurrency, capped at 32 (or one if detection
fails). `--threads 1` executes entirely on the calling thread. Parallel mode
adds one reader thread and uses the calling thread for ordered writing, in
addition to the requested codec workers.

`--max-in-flight` accepts 1 through 1024 blocks; the default upper bound is twice
the worker count. The limit covers a block being read, queued jobs, running work, completed
results, and the block being written. It can be smaller than the worker count.
Lower it to reduce payload memory; increase it to allow more read-ahead. Thread
count and this limit do not change compressed bytes for a fixed block size.

`--buffer-limit` is a positive decimal byte count, defaulting to 256 MiB. It
clamps admission further using a conservative bound for simultaneous input and
result ownership: `2 * block_size` per compression slot and
`5 * block_size + 256` per decompression slot. A budget too small for one slot
is rejected. This bounds live block buffers, not process RSS: codec scratch
space, thread stacks, allocator retention, and stream buffers are extra.

`--max-output` is a decimal byte count valid only for decompression; the default
is unlimited, and zero permits empty output only. The reader checks the running
decoded-size total without integer overflow before allocating each block body.
Use it when processing input whose total expanded size should be bounded.

Input files are preserved. Processing writes into an exclusively created
temporary directory beside the destination. Only a complete, flushed, closed
file is published at the requested path. Existing files, aliases of the input,
and destinations created during processing cannot be overwritten. Handled
failures clean up the owned temporary file and directory. Abrupt termination
can leave a `.huffblock-*` temporary directory, but no partial final output.

Publication uses no-replace rename on Windows and Linux. When Linux does not
support that operation, and on other POSIX systems, the fallback creates a hard
link to the completed file. Filesystems supporting neither operation fail safely.
Publication does not promise persistence across power loss; it does not fsync
the payload and parent directory.

Exit codes are `0` for success, `1` for processing/I/O errors, and `2` for invalid
command-line arguments. Errors are written to stderr. This version takes file
paths; stdin/stdout streaming and directory archiving are outside its CLI scope.

## Design

```text
CLI -> temporary output wrapper -> archive framing
                                      |
               reader -> bounded jobs -> workers -> ordered writer
                                           |
                                  independent block codec
```

- `include/huffblock/codec.hpp`: block-level API with owned results and no file I/O.
- `src/codec.cpp`: deterministic Huffman construction, a 64-bit bit writer,
  a 10-bit decode table with tree fallback, size checks, and checksums. The
  original per-bit paths remain available for reproducible comparison builds.
- `include/huffblock/archive.hpp` and `src/archive.cpp`: framing, execution options,
  stream APIs, and file wrappers.
- `src/ordered_pipeline.hpp`: fixed `std::jthread` workers, a bounded job queue,
  a bounded ring of results, shared stop requests, and error propagation.
- `src/file_output.cpp`: temporary output ownership and publication without replacement.
- `src/limits.hpp`: checked decoded-size accounting.
- `src/main.cpp`: CLI parsing and exit codes.
- `tests/`: independent known-code/format examples, seeded round trips, malformed
  input, stream failures, controlled out-of-order completion, admission limits,
  cancellation, publication races, file preservation, and CLI integration checks.
- `tools/`: seeded benchmarks, gprof profiling, independent fuzzer seeds,
  benchmark summaries, and native Windows/WSL archive interoperability checks.

The encoder counts bytes separately for every block. It breaks equal-frequency
tree choices by the smallest byte value in each subtree, then assigns canonical
codes in code-length/byte-value order. Repeated compression with the same block
size produces identical output. It stores raw bytes whenever the Huffman body,
including its 256-byte length table, would be as large as or larger than the input.
Empty files have no data blocks, only a file header and required end marker.

The reader reserves an admission credit before allocating or reading a block.
That credit is returned only after the ordered writer has written and destroyed
its result. A slow early block therefore cannot cause unlimited later results
to accumulate. Workers own their inputs, codebooks, and output buffers; input
and output streams each have a single owner thread.

With the effective admission limit K and block-size limit B, payload memory is O(K * B), independent of
file length. Compression can briefly hold both an input and result per active
block (up to about 2 * K * B). A legal Huffman body can be as large as 4 * B + 256,
so worst-case decoding payload memory is about K * (5 * B + 256). These bounds
exclude codec scratch space, stream buffers, and thread stacks. Sequential mode
retains only one block at a time.

The first recorded pipeline error requests a shared stop, wakes admission/job/
result waits, and is rethrown on the calling thread after every thread is joined.
Already-running codec calls finish their current bounded block. Standard
blocking stream I/O is not forcibly interrupted. Streams can contain partial
output after an error; the file wrappers provide publication only on success.

Decoding checks declared sizes before allocating the body, validates codebooks
and bit padding, requires the exact decoded length, and verifies the checksum
before writing that block. The mandatory end marker detects truncation at block
boundaries; extra bytes after it are rejected.

The wire format uses explicit integer serialization rather than dumping C++
structures, so host byte order and structure padding do not affect it. See
[the version 1 format specification](docs/FORMAT.md).

## Compression expectations

Bytewise Huffman coding exploits unequal byte frequencies; it does not recognize
repeated strings. Some repetitive files will therefore use raw blocks. Raw
storage avoids expansion of the block body, but every file and block still has
framing overhead. CRC-32 detects accidental corruption and is not authentication.

## Phase completion

All three planned phases are implemented and audited in
[VALIDATION.md](docs/VALIDATION.md). Random-access indexing, directory archiving,
and additional compression algorithms remain outside the agreed scope.

## Performance and verification tools

See [measured throughput, compression ratios, memory, scaling, and block-size
comparisons](docs/BENCHMARKS.md), including raw observations and reproducible
commands. Timing assertions are deliberately absent from correctness tests.

```sh
# Separate GCC/Clang builds on Linux; never benchmark instrumented binaries.
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHUFFBLOCK_SANITIZER=address
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DHUFFBLOCK_SANITIZER=thread
cmake --build build-tsan --parallel 2
setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure

# Optional coverage-guided decoder fuzzing (Clang with compiler-rt/libFuzzer).
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DHUFFBLOCK_SANITIZER=address -DHUFFBLOCK_BUILD_FUZZER=ON
cmake --build build-fuzz --target huffblock_fuzz_decode --parallel 2
python3 tools/fuzz_corpus.py build-fuzz/corpus
./build-fuzz/huffblock_fuzz_decode build-fuzz/corpus -max_total_time=30 -max_len=16384 -rss_limit_mb=512 -timeout=3

# Sampling profile, collected separately from release benchmarks.
cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Release -DHUFFBLOCK_PROFILE=ON
cmake --build build-profile --target huffblock --parallel 2
python3 tools/profile.py --exe build-profile/huffblock --output-prefix profile
```

`address` enables both AddressSanitizer and UndefinedBehaviorSanitizer. `thread`
enables ThreadSanitizer. `setarch -R` disables address randomization only for the
test process and children, avoiding older TSan/WSL startup mapping conflicts;
no system-wide setting is changed. On hosts without that conflict, plain CTest
also works. Sanitizer and profiling modes are mutually exclusive.

The ordinary CTest suite always runs a deterministic mutation/truncation campaign,
even when Clang/libFuzzer is unavailable. Benchmark/profile scripts use Python
3.9+ and its standard library, plus GNU time for Linux benchmark memory
measurement and GNU gprof for profiling. Reports are written to new files
rather than overwriting earlier measurements.

On Windows with Node.js and WSL, run `node tools/verify_interop.mjs` after building
`build-windows/huffblock.exe` and `build-release/huffblock`. It supports
`--windows-exe`, `--linux-exe`, and `--distribution` overrides. Ensure the Windows
compiler's runtime DLLs are beside the executable or on PATH.

[CI configuration](.github/workflows/ci.yml) covers Windows/Linux Debug/Release,
ASan/UBSan, TSan, the reference codec, and a bounded libFuzzer run. The workflow
is supplied for future repository pushes; no remote CI run was triggered here.
