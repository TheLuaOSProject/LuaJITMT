from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

p = Path(__file__).resolve().parent
variant = sys.argv[1]
kind = sys.argv[2]
src = p / variant / "src"
filename = "t-string-retention-recovery.c" if kind == "recovery" else "t-string-retention.c"
lua = p / ("trace-trigger-late.lua" if kind.startswith("trace") else "peer-control.lua")
binary = p / (variant + "-" + kind)
cmd = ["clang" if variant == "asan" else "cc", "-std=gnu11", "-O2", "-g",
       "-Wall", "-Wextra", "-Werror", "-I" + str(src)]
if variant != "normal":
    cmd += ["-DLUA_USE_ASSERT", "-DLUA_USE_APICHECK"]
if variant == "asan":
    cmd += ["-fsanitize=address", "-fno-omit-frame-pointer"]
cmd += [str(p / filename), str(src / "libluajit.a"), "-lm", "-ldl", "-pthread",
        "-Wl,-E", "-o", str(binary)]
env = dict(os.environ)
env["LUA_PATH"] = str(src / "?.lua") + ";;"
env["RETENTION_JIT"] = "1" if kind == "trace" else "0"
if variant == "asan":
    env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
results = []

def run(argv, label):
    label = variant + "-" + kind + "-" + label
    t = time.monotonic()
    with (p / (label + ".stdout")).open("w") as out, \
         (p / (label + ".stderr")).open("w") as err:
        d = subprocess.run(argv, cwd=p, env=env, stdout=out, stderr=err, timeout=50)
    r = {"argv": argv, "cwd": str(p), "exit_code": d.returncode,
         "seconds": time.monotonic() - t, "stdout": label + ".stdout",
         "stderr": label + ".stderr", "LUA_PATH": env["LUA_PATH"],
         "RETENTION_JIT": env["RETENTION_JIT"]}
    if "ASAN_OPTIONS" in env:
        r["ASAN_OPTIONS"] = env["ASAN_OPTIONS"]
    results.append(r)
    (p / (variant + "-" + kind + "-results.json")).write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(r), flush=True)
    return d.returncode

assert run(cmd, "compile") == 0
identities = {str(q.relative_to(p)): {"sha256": hashlib.sha256(q.read_bytes()).hexdigest(),
                                      "bytes": q.stat().st_size}
              for q in [p / filename, lua, binary, src / "libluajit.a"]}
(p / (variant + "-" + kind + "-inputs.json")).write_text(json.dumps(identities, indent=2) + "\n")
for workers in [0, 2]:
    run([str(binary), "0", "1", str(workers), str(lua)], "1-" + str(workers))
