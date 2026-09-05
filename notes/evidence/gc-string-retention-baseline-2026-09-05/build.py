from pathlib import Path
import hashlib
import json
import subprocess
import sys
import time

root = Path(__file__).resolve().parent
variant = sys.argv[1]
cmd = ["make", "-C", "src", "-j4"]
if variant == "strict":
    cmd += ["BUILDMODE=static", "XCFLAGS=-DLUA_USE_ASSERT -DLUA_USE_APICHECK", "TARGET_STRIP=:"]
elif variant == "asan":
    cmd += ["BUILDMODE=static", "CC=clang", "HOST_CC=clang", "CCOPT=-O1",
            "CCDEBUG=-g", "XCFLAGS=-DLUA_USE_ASSERT -DLUA_USE_APICHECK",
            "TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer",
            "TARGET_LDFLAGS=-fsanitize=address", "TARGET_STRIP=:"]
start = time.monotonic()
with (root / (variant + "-build.stdout")).open("w") as out, \
     (root / (variant + "-build.stderr")).open("w") as err:
    done = subprocess.run(cmd, cwd=root / variant, stdout=out, stderr=err, timeout=180)
result = {"argv": cmd, "cwd": str(root / variant), "exit_code": done.returncode,
          "seconds": time.monotonic() - start, "binaries": {}}
for name in ["luajit", "libluajit.a", "libluajit.so", "host/minilua", "host/buildvm", "lj_str.o"]:
    p = root / variant / "src" / name
    if p.exists():
        result["binaries"][name] = {"sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                                     "bytes": p.stat().st_size}
(root / (variant + "-build.json")).write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result))
raise SystemExit(done.returncode)
