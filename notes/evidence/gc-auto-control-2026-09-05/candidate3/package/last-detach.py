from pathlib import Path
import os, sys, subprocess, time, json, hashlib
r=Path(__file__).resolve().parent
v=sys.argv[1]
assert v in ['candidate3','strict'], 'This exact emitted-MOV fixture is GCC-specific.'
s=r/v/'src'
source=r/'t-restart-last-detach.c'
label=v+'-last-detach'
exe=r/(label+'-fixture')
env=dict(os.environ,LUA_PATH=str(s/'?.lua')+';;')
flags=['-DLUA_USE_ASSERT','-DLUA_USE_APICHECK'] if v=='strict' else []
cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',*flags,'-I'+str(s),str(source),str(s/'libluajit.a'),'-lm','-ldl','-pthread','-Wl,-E','-o',str(exe)]
rows=[]
for name,argv in [('compile',cmd),('disassembly',['objdump','-d','--disassemble=threading_gc_leave',str(exe)]),('run',[str(exe)])]:
 st=time.monotonic()
 with (r/(label+'-'+name+'.stdout')).open('w') as out,(r/(label+'-'+name+'.stderr')).open('w') as err:
  try:code=subprocess.run(argv,cwd=r,env=env,stdout=out,stderr=err,timeout=25).returncode
  except subprocess.TimeoutExpired:code='timeout'
 row=dict(name=name,argv=argv,exit_code=code,seconds=time.monotonic()-st,cwd=str(r),LUA_PATH=env['LUA_PATH'],stdout=label+'-'+name+'.stdout',stderr=label+'-'+name+'.stderr')
 rows.append(row)
 (r/(label+'-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
 print(json.dumps(row),flush=True)
 if code:break
(r/(label+'-identities.json')).write_text(json.dumps({str(p.relative_to(r)):dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size) for p in [exe,source,s/'libluajit.a'] if p.exists()},indent=2)+'\n')
raise SystemExit(0 if all(row['exit_code']==0 for row in rows) else 1)
