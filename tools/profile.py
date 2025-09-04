#!/usr/bin/env python3
"""Run a GNU -pg build on single-threaded input and save gprof reports."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import time

from benchmark import generate

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=Path, required=True)
parser.add_argument("--output-prefix", type=Path, required=True)
parser.add_argument("--size-mib", type=int, default=64)
parser.add_argument("--dataset", default="text")
parser.add_argument("--work-dir", type=Path, default=Path("build-profile-data"))
args = parser.parse_args()
executable = args.exe.resolve(strict=True)
prefix = args.output_prefix.resolve()
prefix.parent.mkdir(parents=True, exist_ok=True)
args.work_dir.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="profile-", dir=args.work_dir.resolve()) as directory:
    work = Path(directory)
    source = work / "source.bin"
    encoded = work / "encoded.hfb"
    restored = work / "restored.bin"
    source.write_bytes(generate(args.dataset, args.size_mib * 1024 * 1024, 20261022))
    for operation, input_path, output_path in (
        ("compress", source, encoded), ("decompress", encoded, restored)
    ):
        report_path = Path(str(prefix) + "-" + operation + ".txt")
        if report_path.exists():
            raise FileExistsError(report_path)
        start = time.perf_counter()
        subprocess.run([str(executable), operation, "--threads", "1", str(input_path), str(output_path)],
                       cwd=work, check=True)
        with report_path.open("x", encoding="utf-8") as report:
            report.write(f"Dataset: {args.dataset}; input size: {source.stat().st_size}; "
                         f"workers: 1; instrumented wall seconds: {time.perf_counter() - start:.6f}\n")
            report.flush()
            subprocess.run(["gprof", "-b", str(executable), str(work / "gmon.out")],
                           stdout=report, check=True)
        (work / "gmon.out").unlink()
        print(report_path, flush=True)
    if source.read_bytes() != restored.read_bytes():
        raise RuntimeError("profile round trip failed")
