import pathlib, subprocess, time, json, hashlib, resource, os
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path(__file__).resolve().parent; rows=[]; fixture=r/'t-ffi-callxs-callback-stack.c'
for name in ['base-normal','fix-normal','fix-assert']:
 t=r/name; exe=r/(name+'-callback-stack')
 cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(t/'src'),'-I'+str(t/'tests'),str(fixture),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if name=='fix-assert': cmd[1:1]=['-DLUA_USE_ASSERT','-DLJ_XSAVE_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS']
 p=subprocess.run(cmd,capture_output=True,text=True); assert p.returncode==0,p.stderr
 run=['taskset','-c','0-15',str(exe)]; start=time.monotonic(); env=dict(os.environ,LUA_PATH=str(t/'src/?.lua')+';;')
 p=subprocess.run(run,cwd=t,env=env,capture_output=True,text=True,timeout=30)
 (r/(name+'-callback-stack.stdout')).write_text(p.stdout); (r/(name+'-callback-stack.stderr')).write_text(p.stderr)
 rows.append({'variant':name,'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),'compile':cmd,'command':run,'cwd':str(t),'env_override':{'LUA_PATH':env['LUA_PATH']},'exit':p.returncode,'seconds':time.monotonic()-start,'stdout':p.stdout,'stderr':p.stderr,'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
(r/'stack-results.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows: print(row['variant'],row['exit'],row['stdout'],row['stderr'],flush=True)
