from pathlib import Path
import os, subprocess, time, json, signal
r=Path(__file__).resolve().parent
rows=[]
for v in ['controlhelpers','helpers']:
 tree=r/v
 s=tree/'src'
 exe=r/(v+'-safety-t-func-construction-anchor')
 label=v+'-function-timeout-gdb'
 argv=['gdb','--batch','-ex','set pagination off','-ex','set confirm off','-ex','run','-ex','thread apply all bt','--args',str(exe)]
 env=dict(os.environ,LUA_PATH=str(s/'?.lua')+';'+str(tree/'tests/lib/?.lua')+';;')
 st=time.monotonic()
 with (r/(label+'.stdout')).open('w') as out,(r/(label+'.stderr')).open('w') as err:
  proc=subprocess.Popen(argv,cwd=tree,env=env,stdout=out,stderr=err,start_new_session=True)
  time.sleep(3)
  if proc.poll() is None:os.killpg(proc.pid,signal.SIGINT)
  try:code=proc.wait(timeout=10)
  except subprocess.TimeoutExpired:
   os.killpg(proc.pid,signal.SIGKILL)
   proc.wait()
   code='timeout'
 row=dict(name=label,argv=argv,exit_code=code,seconds=time.monotonic()-st,cwd=str(tree),LUA_PATH=env['LUA_PATH'],interrupted_after_seconds=3,stdout=label+'.stdout',stderr=label+'.stderr')
 rows.append(row)
 (r/'function-timeout-gdb-results.json').write_text(json.dumps(rows,indent=2)+'\n')
 print(json.dumps(row),flush=True)
