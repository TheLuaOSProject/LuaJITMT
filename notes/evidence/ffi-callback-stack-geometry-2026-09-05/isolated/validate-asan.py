import pathlib, subprocess, time, json, os, hashlib, resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path(__file__).resolve().parent; tree=r/'fix-asan'; rows=[]
flags=['-DLUA_USE_ASSERT','-DLJ_XSAVE_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS']
cmd=['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4','BUILDMODE=static','CC=clang','CCDEBUG=-g','CCOPT=-O1','TARGET_STRIP=:', 'XCFLAGS='+' '.join(flags),'TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer','TARGET_LDFLAGS=-fsanitize=address']
start=time.monotonic()
with (r/'asan-build.stdout').open('w') as out,(r/'asan-build.stderr').open('w') as err:
 p=subprocess.run(cmd,cwd=tree,stdout=out,stderr=err,timeout=180)
row={'phase':'build','command':cmd,'cwd':str(tree),'exit':p.returncode,'seconds':time.monotonic()-start}
if not p.returncode:
 row['runtime_sha256']=hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest();row['archive_sha256']=hashlib.sha256((tree/'src/libluajit.a').read_bytes()).hexdigest()
rows.append(row);(r/'asan-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(row),flush=True)
if p.returncode: raise SystemExit(p.returncode)
for fixture in ['t-ffi-callxs-callback-stack.c','t-ffi-callxs-callback.c','t-jit-xsave.c']:
 exe=r/('asan-'+fixture[:-2]);cmd=['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-mcx16','-fsanitize=address','-fno-omit-frame-pointer']+flags+['-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(tree/'tests'/fixture),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 p=subprocess.run(cmd,capture_output=True,text=True); row={'fixture':fixture,'compile':cmd,'compile_exit':p.returncode,'compile_stdout':p.stdout,'compile_stderr':p.stderr}
 if not p.returncode:
  run=['taskset','-c','0-15',str(exe)];overrides={'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','LUA_PATH':str(tree/'src/?.lua')+';;'};start=time.monotonic()
  try:
   p=subprocess.run(run,cwd=tree,env=dict(os.environ,**overrides),capture_output=True,text=True,timeout=90)
   row.update({'command':run,'cwd':str(tree),'env_override':overrides,'exit':p.returncode,'seconds':time.monotonic()-start,'stdout':p.stdout,'stderr':p.stderr,'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
  except subprocess.TimeoutExpired as e:
   row.update({'command':run,'exit':'TIMEOUT','seconds':time.monotonic()-start,'stdout':str(e.stdout),'stderr':str(e.stderr)})
 rows.append(row);(r/'asan-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(row),flush=True)
