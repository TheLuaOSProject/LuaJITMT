from pathlib import Path
import subprocess, json, hashlib, time, sys
r=Path(__file__).resolve().parent
v=sys.argv[1]
assert v in ['runtime','debug']
cmd=['make','-C','src','-j4']
if v=='debug':cmd+=['BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
st=time.monotonic()
with (r/(v+'-build.stdout')).open('w') as out,(r/(v+'-build.stderr')).open('w') as err:
 done=subprocess.run(cmd,cwd=r/v,stdout=out,stderr=err,timeout=180)
res=dict(argv=cmd,cwd=str(r/v),exit_code=done.returncode,seconds=time.monotonic()-st,binaries={})
for n in ['luajit','libluajit.a','libluajit.so','host/buildvm','host/minilua']:
 p=r/v/'src'/n
 if p.exists():res['binaries'][n]=dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
(r/(v+'-build.json')).write_text(json.dumps(res,indent=2)+'\n')
print(json.dumps(res),flush=True)
raise SystemExit(done.returncode)
