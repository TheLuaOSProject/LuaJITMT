from pathlib import Path
import subprocess,json,time,os,sys,hashlib
r=Path(__file__).resolve().parent
v=sys.argv[1]; label=v+'-stop-v2'; src=r/v/'src'; binary=r/(label+'-exe')
env=dict(os.environ); env['LUA_PATH']=str(src/'?.lua')+';;'
cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-I'+str(src),str(r/'t-worker-bridge-stop.c'),str(src/'libluajit.a'),'-lm','-ldl','-pthread','-Wl,-E','-Wl,--wrap=lj_safepoint_handshake','-o',str(binary)]
results=[]
def run(cmd,case):
 st=time.monotonic()
 with (r/(label+'-'+case+'.stdout')).open('w') as out,(r/(label+'-'+case+'.stderr')).open('w') as err:
  try:
   p=subprocess.run(cmd,cwd=r,env=env,stdout=out,stderr=err,timeout=35);res=dict(exit_code=p.returncode)
  except subprocess.TimeoutExpired:res=dict(timeout=True)
 res.update(argv=cmd,case=case,cwd=str(r),seconds=time.monotonic()-st,LUA_PATH=env['LUA_PATH'])
 results.append(res);(r/(label+'-results.json')).write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(res),flush=True)
 return res.get('exit_code')==0
assert run(cmd,'compile')
ids={str(p):dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size) for p in [binary,r/'t-worker-bridge-stop.c',src/'libluajit.a']}
(r/(label+'-inputs.json')).write_text(json.dumps(ids,indent=2)+'\n')
passed=True
for case in sys.argv[2:] or [f'{a}-{w}-{l}' for a in [0,1] for w in [1,2] for l in [0,1]]:
 passed &= run([str(binary),*case.split('-')],case)
raise SystemExit(0 if passed else 1)
