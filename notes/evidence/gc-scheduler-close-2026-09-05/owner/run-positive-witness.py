from pathlib import Path
import json,hashlib,subprocess,time,os,resource
P=Path(__file__).resolve().parent;inputs=json.loads((P/'positive-witness-inputs.json').read_text());S=Path(inputs['runtime'])
env=os.environ.copy();env['LUA_PATH']=str(S/'src/?.lua')+';'+str(S/'tests/lib/?.lua')+';;';env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
argv=['gdb','-q','-batch','-x',str(P/'positive-witness.gdb'),'--args',inputs['exe']]
start=time.monotonic();timed=False
with (P/'positive-witness.stdout').open('wb') as out,(P/'positive-witness.stderr').open('wb') as err:
 try:r=subprocess.run(argv,cwd=S,env=env,stdout=out,stderr=err,timeout=60);code=r.returncode
 except subprocess.TimeoutExpired:timed=True;code=124
row=dict(argv=argv,cwd=str(S),LUA_PATH=env['LUA_PATH'],ASAN_OPTIONS=env['ASAN_OPTIONS'],timeout_seconds=60,exit_code=code,timed_out=timed,seconds=time.monotonic()-start,stdout='positive-witness.stdout',stderr='positive-witness.stderr',purpose='read-only successful real-close witness; debugger terminates stopped inferior after lua_close returns; not a fixture pass')
(P/'positive-witness-results.json').write_text(json.dumps(row,indent=2)+'\n');print(json.dumps(row),flush=True)
