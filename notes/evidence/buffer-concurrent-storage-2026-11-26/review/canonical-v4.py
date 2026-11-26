from pathlib import Path
import hashlib,json,os,resource,subprocess,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;root=Path('/workspaces/lj-lockless');out=p/'canonical-v4';out.mkdir(exist_ok=False)
files=subprocess.check_output(['git','ls-files','--','src','dynasm','Makefile','tests/suites'],cwd=root,text=True).splitlines()+['tests/t-buffer-progress.c','tests/t-buffer-concurrent-public.lua']
inputs={f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in files}
(out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
env=os.environ.copy();env.update(LUA_PATH=str(p/'gcc-v4/src/?.lua')+';;',JOBS='6')
cmd=['taskset','-c','0-7,9-15',str(p/'gcc-v4/src/luajit'),'tools/test.lua','m5_buffer_publish','m5_buffer_progress'];t=time.monotonic()
with (out/'stdout').open('wb') as so,(out/'stderr').open('wb') as se:
 code=subprocess.run(cmd,cwd=root,env=env,stdout=so,stderr=se,timeout=180).returncode
meta=dict(command=cmd,exit=code,seconds=time.monotonic()-t,environment={'LUA_PATH':env['LUA_PATH'],'JOBS':env['JOBS']})
meta['outputs']={f:hashlib.sha256((root/'src'/f).read_bytes()).hexdigest() for f in ['luajit','libluajit.a','libluajit.so']}
meta['source_mismatches']=[f for f,h in inputs.items() if hashlib.sha256((root/f).read_bytes()).hexdigest()!=h]
(out/'result.json').write_text(json.dumps(meta,indent=2)+'\n');print('canonical-v4',code,round(meta['seconds'],3),meta['source_mismatches'],flush=True)
raise SystemExit(code)
