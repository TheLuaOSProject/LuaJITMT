from pathlib import Path
import subprocess,os,json,time,signal
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');v='base-normal';env=os.environ.copy();env['LUA_PATH']=str(r/v/'src/?.lua')+';;'
cmd=['taskset','-c','0-15',str(r/(v+'-global-worker'))]
with (r/'global-worker-diagnostic.stdout').open('w') as out,(r/'global-worker-diagnostic.stderr').open('w') as err:
 p=subprocess.Popen(cmd,env=env,stdout=out,stderr=err)
 time.sleep(2)
 if p.poll() is None:
  gd=['gdb','-q','-batch','-p',str(p.pid),'-ex','set pagination off','-ex','thread apply all bt 20','-ex','detach']
  z=subprocess.run(gd,capture_output=True,text=True,timeout=15)
  (r/'global-worker-diagnostic.gdb').write_text(z.stdout+z.stderr)
  p.kill();p.wait()
 else:gd=[]
(r/'global-worker-diagnostic.json').write_text(json.dumps({'command':cmd,'pid':p.pid,'sample_after_seconds':2,'gdb_command':gd,'termination':'killed exact diagnostic process after sample','exit':p.returncode},indent=2)+'\n')
print((r/'global-worker-diagnostic.gdb').read_text())
