from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

root = Path(__file__).resolve().parent
variant = sys.argv[1]
cases = sys.argv[2:] or [f"{e}-{p}-{w}" for e in [1, 0] for p in [0, 1] for w in [0, 2]]
suffix = os.environ.get("RETENTION_RUN_SUFFIX", "")
label = variant + suffix
src = root / variant / "src"
fixture = root / (label + "-retention")
cmd = ["clang" if variant == "asan" else "cc", "-std=gnu11", "-O2", "-g",
       "-Wall", "-Wextra", "-Werror", "-I" + str(src)]
if variant != "normal":
    cmd += ["-DLUA_USE_ASSERT", "-DLUA_USE_APICHECK"]
if variant == "asan":
    cmd += ["-fsanitize=address", "-fno-omit-frame-pointer"]
cmd += [str(root / "t-string-retention.c"), str(src / "libluajit.a"),
        "-lm", "-ldl", "-pthread", "-Wl,-E", "-o", str(fixture)]
results = []

def run(argv, case, env):
    started = time.monotonic()
    with (root / (label + "-" + case + ".stdout")).open("w") as out, \
         (root / (label + "-" + case + ".stderr")).open("w") as err:
        try:
            done = subprocess.run(argv, cwd=root, env=env, stdout=out, stderr=err, timeout=50)
            result = {"exit_code": done.returncode}
        except subprocess.TimeoutExpired:
            result = {"timeout": True}
    result.update({"argv": argv, "case": case, "cwd": str(root),
                   "seconds": time.monotonic() - started,
                   "stdout": label + "-" + case + ".stdout",
                   "stderr": label + "-" + case + ".stderr"})
    if "ASAN_OPTIONS" in env:
        result["ASAN_OPTIONS"] = env["ASAN_OPTIONS"]
    results.append(result)
    (root / (label + "-results.json")).write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    return result.get("exit_code") == 0

env = dict(os.environ)
env["LUA_PATH"] = str(src / "?.lua") + ";;"
if variant == "asan":
    env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
if not run(cmd, "compile", env):
    raise SystemExit(1)
identities = {p.name: {"sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                        "bytes": p.stat().st_size}
              for p in [fixture, root / "t-string-retention.c", root / "peer-control.lua",
                        src / "libluajit.a"]}
(root / (label + "-inputs.json")).write_text(json.dumps(identities, indent=2) + "\n")
passed = True
for case in cases:
    passed &= run([str(fixture), *case.split("-"), str(root / "peer-control.lua")], case, env)
raise SystemExit(0 if passed else 1)
