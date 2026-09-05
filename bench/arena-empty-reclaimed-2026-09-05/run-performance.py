#!/usr/bin/env python3
"""GC-enabled, bounded fresh-process pairs against one immutable baseline."""
import datetime
import json
import os
from pathlib import Path
import resource
import subprocess
import time

OUT = Path(__file__).resolve().parent
BINARIES = {
    "before": "/tmp/lj-gc-jit-combined-20260905-6cxpl6mp/normal/src/luajit",
    "after": "/tmp/lj-empty-reclaimed-20260905-9vnax2uo/normal/src/luajit",
}
HARNESS = OUT.parent / "closure-upvalue-diagnosis-2026-09-04/sequence.lua"
records = []
for condition, samples, selected in [
    ("filtered", 3, "closures_upval"),
    ("after-insertion", 7, "tab_insert_newkey,closures_upval"),
]:
    for sample in range(1, samples + 1):
        order = ["before", "after"] if sample % 2 else ["after", "before"]
        for kind in order:
            exe = BINARIES[kind]
            src = str(Path(exe).parent)
            env = os.environ.copy()
            env.pop("BENCH_GC_MODE", None)
            env.update(BENCH_SCALE="1", CLOSURE_SCALE="0.1",
                       DIAG_STOP_AFTER="closures_upval",
                       LUA_PATH=src + "/?.lua;" + src + "/?/init.lua;;",
                       LUA_CPATH=src + "/?.so;;")
            cmd = ["taskset", "-c", "30", exe, "-jon", "-e",
                   'io.stdout:setvbuf("line")', str(HARNESS), selected]
            started = datetime.datetime.now(datetime.timezone.utc).isoformat()
            usage0 = resource.getrusage(resource.RUSAGE_CHILDREN)
            t0 = time.monotonic()
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True, env=env)
            status = "complete"
            try:
                stdout, stderr = p.communicate(timeout=45)
            except subprocess.TimeoutExpired:
                status = "timeout"
                p.kill()
                stdout, stderr = p.communicate()
            usage1 = resource.getrusage(resource.RUSAGE_CHILDREN)
            prefix = f"{condition}-{sample:02d}-{kind}"
            (OUT / (prefix + ".stdout")).write_text(stdout)
            (OUT / (prefix + ".stderr")).write_text(stderr)
            record = {
                "condition": condition, "sample": sample, "kind": kind,
                "command": cmd, "started_utc": started, "status": status,
                "timeout_s": 45, "exit_code": p.returncode,
                "wall_s": time.monotonic() - t0,
                "user_s": usage1.ru_utime - usage0.ru_utime,
                "system_s": usage1.ru_stime - usage0.ru_stime,
                "environment": {k: env.get(k) for k in [
                    "BENCH_SCALE", "CLOSURE_SCALE", "BENCH_GC_MODE",
                    "DIAG_STOP_AFTER", "LUA_PATH", "LUA_CPATH"]},
                "stdout": prefix + ".stdout", "stderr": prefix + ".stderr",
            }
            records.append(record)
            (OUT / "performance-runs.json").write_text(
                json.dumps(records, indent=2) + "\n")
            print(condition, sample, kind, status, p.returncode,
                  stdout.strip(), flush=True)
