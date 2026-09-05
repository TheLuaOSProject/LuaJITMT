from pathlib import Path
import os, sys, subprocess, time, json, hashlib
r=Path(__file__).resolve().parent
v=sys.argv[1]
tree=r/v
s=tree/'src'
label=v+'-spawn-query'
source=r/'t-finalizer-spawn-query-overlap.lua'
rows=[]
env=dict(os.environ,LUA_PATH=str(s/'?.lua')+';;')
if v=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
for off in [True,False]:
 name='joff' if off else 'jit'
 argv=[str(s/'luajit'),*(['-joff'] if off else []),str(source),'0' if v=='control' else '1']
 st=time.monotonic()
 with (r/(label+'-'+name+'.stdout')).open('w') as out,(r/(label+'-'+name+'.stderr')).open('w') as err:
  try:code=subprocess.run(argv,cwd=tree,env=env,stdout=out,stderr=err,timeout=30).returncode
  except subprocess.TimeoutExpired:code='timeout'
 row=dict(argv=argv,name=name,cwd=str(tree),LUA_PATH=env['LUA_PATH'],ASAN_OPTIONS=env.get('ASAN_OPTIONS'),exit_code=code,seconds=time.monotonic()-st,stdout=label+'-'+name+'.stdout',stderr=label+'-'+name+'.stderr')
 rows.append(row)
 (r/(label+'-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
 print(json.dumps(row),flush=True)
(r/(label+'-identities.json')).write_text(json.dumps({str(p.relative_to(r)):dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size) for p in [source,s/'luajit',s/'libluajit.a']},indent=2)+'\n')
raise SystemExit(0 if all(row['exit_code']==0 for row in rows) else 1)
