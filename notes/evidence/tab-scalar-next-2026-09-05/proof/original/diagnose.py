from pathlib import Path
import hashlib,json,os,resource,subprocess,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;tree=Path('/tmp/lj-clib-cdata-combined-20260905-bxrxos7h/strict')
envadd={'LUA_PATH':str(tree/'src/?.lua')+';;'}
cmd=['gdb','-q','-batch','-x',str(p/'diagnose.gdb'),'--args',str(p/'fixture')]
start=time.monotonic();q=subprocess.run(cmd,cwd=p,env={**os.environ,**envadd},capture_output=True,text=True,timeout=18)
(p/'diagnose.stdout').write_text(q.stdout);(p/'diagnose.stderr').write_text(q.stderr)
(p/'diagnose.json').write_text(json.dumps({'command':cmd,'environment':envadd,'seconds':time.monotonic()-start,'exit':q.returncode,'binary_sha256':hashlib.sha256((p/'fixture').read_bytes()).hexdigest(),'note':'Three-second debugger interruption is evidence capture, never a fixture pass. Actual source unchanged; strict debug archive differs from original canonical GC2-only binary.'},indent=2)+'\n');print('diagnostic',q.returncode,flush=True)
