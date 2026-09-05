from pathlib import Path
import subprocess,os,json,time
p=Path(__file__).parent;c=p/'canonical-lua';env=os.environ.copy();env['LJ_TEST_ROOT']=str(c);env['JOBS']='4';cmd=['taskset','-c','0-15',str(p/'normal/src/luajit'),str(c/'tools/test.lua'),'m5_meta_cdata_capture_joff','m5_meta_cdata_capture'];start=time.monotonic()
with (p/'canonical-lua.stdout').open('w') as o,(p/'canonical-lua.stderr').open('w') as e:r=subprocess.run(cmd,env=env,cwd=c,stdout=o,stderr=e,timeout=120)
rec={'command':cmd,'cwd':str(c),'env':{'LJ_TEST_ROOT':str(c),'JOBS':'4'},'exit':r.returncode,'seconds':time.monotonic()-start,'limit_seconds':120};(p/'canonical-lua.json').write_text(json.dumps(rec,indent=2)+'\n');print(json.dumps(rec),flush=True);assert r.returncode==0
