from pathlib import Path
import subprocess,time,json,os,hashlib
p=Path(__file__).resolve().parent;root=Path('/workspaces/lj-lockless')
env=os.environ.copy();tmp=p/'canonical-tmp';tmp.mkdir(exist_ok=True)
env.update({'LJ_TEST_ROOT':str(root),'JOBS':'4','MAKE_JOBS':'4','TMPDIR':str(tmp),'LUA_PATH':str(root/'src/?.lua')+';;'})
cmd=['taskset','-c','0-15','/tmp/lj-special-udata-independent-20260905-75jums60/strict/src/luajit',str(root/'tools/test.lua'),'m7_ffi_clib_receiver']
start=time.monotonic();r=subprocess.run(cmd,cwd=root,env=env,capture_output=True,text=True,timeout=120)
(p/'canonical.stdout').write_text(r.stdout);(p/'canonical.stderr').write_text(r.stderr)
row={'command':cmd,'cwd':str(root),'environment':{k:env[k] for k in ['LJ_TEST_ROOT','JOBS','MAKE_JOBS','TMPDIR','LUA_PATH']},'exit':r.returncode,'seconds':time.monotonic()-start,'sources':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in ['src/lj_crecord.c','tests/suites/m7_ffi.lua','tests/t-ffi-clib-receiver.lua','tests/t-ffi-clib-receiver-lib.c']},'default_binaries':{f:hashlib.sha256((root/'src'/f).read_bytes()).hexdigest() for f in ['luajit','libluajit.a','libluajit.so']}}
(p/'canonical.json').write_text(json.dumps(row,indent=2)+'\n');print(json.dumps(row));assert r.returncode==0,r.stderr
