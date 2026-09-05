from pathlib import Path
import hashlib,json,os,resource,subprocess,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
rows=[]
for run in range(4):
 tree=p/'strict'
 cmd=['taskset','-c','0-15','gdb','-q','-batch',
      '-ex','set pagination off','-ex','set print thread-events off',
      '-ex','set disable-randomization off','-ex','run',
      '-ex','thread apply all bt 24','-ex','info registers',
      '-ex','x/16i $pc-16','-ex','info proc mappings',
      '--args',str(tree/'src/luajit'),'-jon','test.lua','--quiet']
 env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;';env.pop('ASAN_OPTIONS',None)
 a=time.monotonic();r=subprocess.run(cmd,cwd=tree/'tests/stock/test',env=env,capture_output=True,text=True,timeout=50)
 row={'command':cmd,'cwd':str(tree/'tests/stock/test'),'environment':{'LUA_PATH':env['LUA_PATH']},'seconds':time.monotonic()-a,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr,'executable_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest()}
 rows.append(row);(p/'diagnosis-v2/stock-strict-aslr-gdb.json').write_text(json.dumps(rows,indent=2)+'\n')
 signal=next((line for line in r.stdout.splitlines() if 'received signal' in line),None)
 print(run,signal or ('completed' if '509 passed' in r.stdout else 'other'),flush=True)
 if signal:break
