import pathlib,subprocess,os,time,json,hashlib,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path(__file__).resolve().parent;t=r/'strict';tests=r/'tree/tests';rows=[];baseenv={'LUA_PATH':str(t/'src/?.lua')+';;'}
cases=[]
for mode in ['index','newindex','missing','nonfunction','resize','methodlife','replace']:cases.append(('mutation-'+mode,['t-jit-cdata-pure.lua',mode]))
cases.append(('side',['t-jit-cdata-pure-side.lua']))
for mode in ['allocate','luastore','newref','clear','foreign','indirect','fpmath']:cases.append(('exclude-'+mode,['t-jit-cdata-pure-exclusions.lua',mode]))
cases.append(('profile',['t-jit-cdata-pure-profile.lua']))
for name,args in cases:
 cmd=['taskset','-c','0-15',str(t/'src/luajit'),'-jon',str(tests/args[0])]+args[1:];start=time.monotonic()
 p=subprocess.run(cmd,cwd=t,env=dict(os.environ,**baseenv),capture_output=True,text=True,timeout=20)
 row={'case':name,'command':cmd,'cwd':str(t),'env_override':baseenv,'exit':p.returncode,'seconds':time.monotonic()-start,'stdout':p.stdout,'stderr':p.stderr};rows.append(row);(r/'strict-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,p.returncode,p.stderr[:150],flush=True)
for kind in ['phase','error']:
 f=tests/('t-jit-cdata-pure-'+kind+'.c');exe=r/('strict-'+kind)
 cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-DLUA_USE_ASSERT','-I'+str(t/'src'),str(f),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread']
 if kind=='error':cmd+=['-Wl,--wrap=lj_mem_realloc']
 cmd+=['-o',str(exe)];p=subprocess.run(cmd,capture_output=True,text=True);assert p.returncode==0,p.stderr
 for mode in (['gate','worker'] if kind=='phase' else ['']):
  run=['taskset','-c','0-15',str(exe)]+([mode] if mode else []);start=time.monotonic()
  p=subprocess.run(run,cwd=t,env=dict(os.environ,**baseenv),capture_output=True,text=True,timeout=20)
  row={'case':kind+'-'+mode,'compile':cmd,'command':run,'cwd':str(t),'env_override':baseenv,'exit':p.returncode,'seconds':time.monotonic()-start,'stdout':p.stdout,'stderr':p.stderr,'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()};rows.append(row);(r/'strict-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(kind,mode,p.returncode,p.stdout,p.stderr,flush=True)
