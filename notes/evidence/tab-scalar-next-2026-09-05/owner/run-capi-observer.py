from pathlib import Path
import hashlib,json,os,subprocess,time
p=Path(__file__).resolve().parent
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
out=p/'capi-observer.json'
assert not out.exists()
binary=p/'validation/progress-candidate-v1/fixture'
cmd=['taskset','-c','24-25','gdb','-q','-batch','-x',str(p/'capi-observer.gdb'),'--args',str(binary),'capi','dense']
start=time.monotonic()
q=subprocess.run(cmd,cwd=p,capture_output=True,text=True,timeout=15)
out.write_text(json.dumps({'command':cmd,'cwd':str(p),'exit':q.returncode,'seconds':time.monotonic()-start,'stdout':q.stdout,'stderr':q.stderr,'inputs':{str(f):sha(f) for f in [p/'run-capi-observer.py',p/'capi-observer.gdb',binary,p/'fixtures/t-tab-scalar-next-progress.c']},'note':'Read-only debugger interruption; not a runtime pass. Separate actual unmodified capi-mode SIGALRM retained in progress-candidate-v1/results.json.'},indent=2)+'\n')
print(q.stdout)
