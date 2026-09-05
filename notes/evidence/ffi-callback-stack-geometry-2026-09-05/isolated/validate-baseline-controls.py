import pathlib, subprocess, time, json, os, hashlib, resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path(__file__).resolve().parent; t=r/'base-assert'; rows=[]
flags=['-DLUA_USE_ASSERT','-DLJ_XSAVE_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS']
cmd=['taskset','-c','0-15','make','-C',str(t/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:', 'XCFLAGS='+' '.join(flags)]
start=time.monotonic()
with (r/'base-assert-build.stdout').open('w') as out,(r/'base-assert-build.stderr').open('w') as err:p=subprocess.run(cmd,cwd=t,stdout=out,stderr=err,timeout=120)
rows.append({'case':'build','command':cmd,'exit':p.returncode,'seconds':time.monotonic()-start});assert p.returncode==0
for name in ['base-assert','fix-assert']:
 tree=r/name;exe=r/(name+'-jit-xsave')
 cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(tree/'src'),str(tree/'tests/t-jit-xsave.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 p=subprocess.run(cmd,capture_output=True,text=True);assert p.returncode==0,p.stderr
 run=['taskset','-c','0-15',str(exe)];overrides={'LUA_PATH':str(tree/'src/?.lua')+';;'};start=time.monotonic()
 p=subprocess.run(run,cwd=tree,env=dict(os.environ,**overrides),capture_output=True,text=True,timeout=30)
 row={'case':'xsave','variant':name,'compile':cmd,'command':run,'cwd':str(tree),'env_override':overrides,'exit':p.returncode,'seconds':time.monotonic()-start,'stdout':p.stdout,'stderr':p.stderr,'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()};rows.append(row);print(json.dumps(row),flush=True)
so=r/'control-callxs-remote-flush.so';cmd=['cc','-std=gnu11','-O2','-fPIC','-shared',str(t/'tests/t-ffi-callxs-remote-flush-lib.c'),'-pthread','-o',str(so)]
p=subprocess.run(cmd,capture_output=True,text=True);assert p.returncode==0,p.stderr
for name in ['base-assert','fix-assert']:
 tree=r/name;run=['taskset','-c','0-15',str(tree/'src/luajit'),str(tree/'tests/t-ffi-callxs-remote-flush.lua')];overrides={'LUA_PATH':str(tree/'src/?.lua')+';;','LJ_M7_FFI_CALLXS_FLUSH_SO':str(so)};start=time.monotonic()
 try:
  p=subprocess.run(run,cwd=tree,env=dict(os.environ,**overrides),capture_output=True,text=True,timeout=35);row={'case':'remote-flush','variant':name,'command':run,'cwd':str(tree),'env_override':overrides,'exit':p.returncode,'seconds':time.monotonic()-start,'stdout':p.stdout,'stderr':p.stderr}
 except subprocess.TimeoutExpired as e:row={'case':'remote-flush','variant':name,'command':run,'cwd':str(tree),'env_override':overrides,'exit':'TIMEOUT','seconds':time.monotonic()-start,'stdout':str(e.stdout),'stderr':str(e.stderr)}
 rows.append(row);(r/'baseline-control-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(row),flush=True)
(r/'baseline-control-results.json').write_text(json.dumps(rows,indent=2)+'\n')
