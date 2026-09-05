import pathlib, subprocess, time, json, os, resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path(__file__).resolve().parent; tree=r/'canonical'; rows=[]
for case in ['m7_ffi_callxs_authentic','m7_ffi_callback_runtime','m7_ffi_ccall_native','m6_jit_xsave','m7_ffi_native_frames']:
 cmd=['taskset','-c','0-15',str(r/'base-normal/src/luajit'),str(tree/'tools/test.lua'),case]
 overrides={'LJ_TEST_ROOT':str(tree),'JOBS':'4','MAKE_JOBS':'4','LUA_PATH':str(tree/'src/?.lua')+';;'}
 start=time.monotonic()
 with (r/('canonical-'+case+'.stdout')).open('w') as out,(r/('canonical-'+case+'.stderr')).open('w') as err:
  try: p=subprocess.run(cmd,cwd=tree,env=dict(os.environ,**overrides),stdout=out,stderr=err,timeout=180); code=p.returncode
  except subprocess.TimeoutExpired: code='TIMEOUT'
 row={'case':case,'command':cmd,'cwd':str(tree),'env_override':overrides,'exit':code,'seconds':time.monotonic()-start};rows.append(row)
 (r/'canonical-results.json').write_text(json.dumps(rows,indent=2)+'\n')
 print(json.dumps(row),flush=True)
