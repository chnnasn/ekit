"""Run fresh-process Boids matrices; archive output before analyzing it."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("binary_directory", type=Path)
parser.add_argument("--threads", default="1,2,3,4,24")
parser.add_argument("--tag", default="")
args = parser.parse_args()
out = Path(__file__).resolve().parent / "boids_retest_raw"
out.mkdir(exist_ok=True)
options = ["--boids", "200,500,1000,2000,5000,10000", "--threads", args.threads,
           "--steps", "120", "--warmup", "20", "--seed", "20260810",
           "--width", "800", "--height", "600"]
for run in range(6):
    modes = ["scheduler", "parallel"] if run % 2 == 0 else ["parallel", "scheduler"]
    for mode in modes:
        exe = "ekit_boids_bench" + ("_parallel" if mode == "parallel" else "") + ".exe"
        print(f"Starting round {run}: {mode}", flush=True)
        result = subprocess.run([str(args.binary_directory.resolve() / exe), *options],
                                capture_output=True, text=True, check=True)
        (out / f"{mode}_{run}{args.tag}.txt").write_text(result.stdout, encoding="utf-8")
        if "DIFF" in result.stdout:
            raise RuntimeError(f"State checksum mismatch in {mode} round {run}")
        print(f"Finished round {run}: {mode}", flush=True)
