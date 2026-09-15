#!/usr/bin/env python3
"""Build the Scratch Everywhere! PAPP for the host and run it on the test project.

Uses tools/build_papp.py to fetch exactly what the .papp is built from (the
pinned upstream, its pinned dependencies and ports/scratch/patches), takes
the same compile units and flags from apps/psram_scratch/papp.json, and
compiles them with the host's gcc/g++ instead of riscv32-esp-elf-*: without
the RISC-V flags, the newlib glue (papp_syscalls.c) and app_entry
(papp_main.cpp), plus host_main.cpp, a fake loader that scripts input and
checks the runtime's state (see its header comment).

The app reads its projects from /sd/scratch/, as on the device, so that
folder must exist and be writable; the test project is copied there.

    python3 apps/psram_scratch/tests/host/run_host_test.py [--out DIR] [--no-asan]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import build_papp as bp  # noqa: E402

TARGET_ONLY = {"papp_main.cpp", "papp_syscalls.c"}
TARGET_FLAGS = ("-march=", "-mabi=", "-mcmodel=")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=Path.cwd() / "scratch-host", help="build output and frame PNGs")
    parser.add_argument("--cache", type=Path, default=ROOT / ".cache", help="upstream checkout cache")
    parser.add_argument("--no-asan", action="store_true", help="build without AddressSanitizer")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    manifest = json.loads((ROOT / "apps/psram_scratch/papp.json").read_text())
    cache = args.cache.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    # The same checkout build_papp.build_app() makes.
    source = manifest["source"]
    subs = bp.check_submodules(manifest["name"], manifest)
    sparse = [source["path"], *bp.SDK_PATHS, *bp.manifest_paths(manifest)]
    sparse = [p for p in sparse if not p.startswith(bp.LOCAL_PREFIX) and p not in ("", ".")]
    sparse, sub_sparse = bp.split_sparse(sparse, subs)
    src = bp.fetch(source["repo"], source["ref"], cache / f"src-{manifest['name']}-{source['ref'][:12]}", sparse)
    bp.fetch_submodules(src, subs, sub_sparse)
    print("patches:", bp.apply_patches(src, manifest["patches"], [s["path"] for s in subs]), flush=True)

    build = out / "obj"
    units, _ = bp.custom_units(manifest, src, build)
    host_flags = ["-g", "-O1", "-I" + str(HERE / "include")]
    if not args.no_asan:
        host_flags.append("-fsanitize=address")
    compiled = []
    for unit in units:
        if unit.src.name in TARGET_ONLY:
            continue
        flags = [f for f in unit.flags if not f.startswith(TARGET_FLAGS)]
        compiler = "g++" if unit.compiler == bp.CXX else "gcc"
        compiled.append((compiler, host_flags + flags, unit.src, unit.obj))
    # The fake loader, with the port's includes (the last unit is the port's).
    port_flags = next(flags for compiler, flags, s, _ in compiled if s.name == "papp_scratch.cpp")
    compiled.append(("g++", port_flags, HERE / "host_main.cpp", build / "host_main.o"))

    failures = []

    def compile_one(job) -> None:
        compiler, flags, source_file, obj = job
        obj.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([compiler, *flags, "-c", "-o", str(obj), str(source_file)], capture_output=True,
                                text=True)
        if result.returncode != 0:
            failures.append(f"{source_file}\n{result.stderr}")

    print(f"compiling {len(compiled)} files for the host", flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(compile_one, compiled))
    if failures:
        for failure in failures[:20]:
            print(failure[:4000], flush=True)
        print(f"{len(failures)} file(s) failed to compile")
        return 1

    binary = out / "scratch_host_test"
    link = ["g++", "-g", *(["-fsanitize=address"] if not args.no_asan else []), "-o", str(binary),
            *[str(obj) for _, _, _, obj in compiled], "-lm", "-lpthread"]
    subprocess.run(link, check=True)

    projects = Path("/sd/scratch")
    projects.mkdir(parents=True, exist_ok=True)
    for old in projects.glob("*.sb3"):
        old.unlink()
    shutil.copy(ROOT / "apps/psram_scratch/tests/papp_test.sb3", projects / "papp_test.sb3")

    env = {**os.environ, "SCRATCH_HOST_OUT": str(out), "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1"}
    print("running", binary, flush=True)
    result = subprocess.run([str(binary)], env=env, timeout=600)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
