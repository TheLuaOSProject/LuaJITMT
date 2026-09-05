import pathlib,subprocess,os,time,json,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path(__file__).resolve().parent;t=r/'tree';rows=[]
for case in ['m6_jit_cdata_pure','m6_jit_cdata_pure_lifecycle']:
 cmd=['taskset','-c','0-15',str(r/'strict/src/luajit'),str(t/'tools/test.lua'),case]
 o={'LJ_TEST_ROOT':str(t),'JOBS':'4','MAKE_JOBS':'4','TMPDIR':str(r/'tmp'),'LUA_PATH':str(t/'src/?.lua')+';;'};start=time.monotonic()
 with (r/('canonical-'+case+'.stdout')).open('w') as out,(r/('canonical-'+case+'.stderr')).open('w') as err:p=subprocess.run(cmd,cwd=t,env=dict(os.environ,**o),stdout=out,stderr=err,timeout=120)
 row={'case':case,'command':cmd,'cwd':str(t),'env_override':o,'exit':p.returncode,'seconds':time.monotonic()-start};rows.append(row);(r/'canonical-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(row),flush=True)
