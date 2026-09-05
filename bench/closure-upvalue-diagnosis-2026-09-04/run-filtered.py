#!/usr/bin/env python3
"""Bounded fresh-process samples of the unchanged closure benchmark."""
import datetime, json, os, resource, subprocess, time
from pathlib import Path
out=Path(__file__).resolve().parent
binaries={
  "leaf": "/tmp/lj-sweep-coalesce-review-20260904-2cpu_bml/tree-leaf/src/luajit",
  "stock": "/tmp/lj-runtime-performance-review-2026-09-04/stock/src/luajit",
}
harness="/tmp/lj-sweep-coalesce-review-20260904-2cpu_bml/tree-leaf/plan/aux/bench/bench.lua"
records=[]
for sample in range(3):
  for kind in (["stock","leaf"] if sample%2==0 else ["leaf","stock"]):
    exe=binaries[kind]; src=str(Path(exe).parent)
    env=os.environ.copy()
    env.pop("BENCH_GC_MODE",None)
    env.update(BENCH_SCALE="0.1",LUA_PATH=src+"/?.lua;"+src+"/?/init.lua;;",LUA_CPATH=src+"/?.so;;")
    cmd=["taskset","-c","30",exe,"-jon","-e",'io.stdout:setvbuf("line")',harness,"closures_upval"]
    started=datetime.datetime.now(datetime.timezone.utc).isoformat()
    before=resource.getrusage(resource.RUSAGE_CHILDREN); t0=time.monotonic()
    p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
    status="complete"
    try: stdout,stderr=p.communicate(timeout=45)
    except subprocess.TimeoutExpired:
      status="timeout";p.kill();stdout,stderr=p.communicate()
    after=resource.getrusage(resource.RUSAGE_CHILDREN)
    record={"sample":sample,"kind":kind,"scale":0.1,"iters":500000,"command":cmd,"environment":{k:env.get(k) for k in ["BENCH_SCALE","BENCH_GC_MODE","LUA_PATH","LUA_CPATH"]},"started_utc":started,"status":status,"timeout_s":45,"exit_code":p.returncode,"wall_s":time.monotonic()-t0,"user_s":after.ru_utime-before.ru_utime,"system_s":after.ru_stime-before.ru_stime}
    prefix=f"filtered-{sample+1:02d}-{kind}"
    (out/(prefix+".stdout")).write_text(stdout);(out/(prefix+".stderr")).write_text(stderr)
    record["stdout"]=prefix+".stdout";record["stderr"]=prefix+".stderr"
    records.append(record);(out/"filtered-runs.json").write_text(json.dumps(records,indent=2)+"\n")
    print(kind,sample,status,p.returncode,stdout.strip(),flush=True)
