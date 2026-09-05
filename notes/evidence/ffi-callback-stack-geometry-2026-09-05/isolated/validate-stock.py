import pathlib, subprocess, time, json, os, resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path(__file__).resolve().parent; tree=r/'fix-normal'; rows=[]
for mode in ['-joff','-jon']:
 cmd=['taskset','-c','0-15',str(tree/'src/luajit'),mode,'test.lua','--quiet']; overrides={'LUA_PATH':str(tree/'src/?.lua')+';;'}
 start=time.monotonic()
 p=subprocess.run(cmd,cwd=tree/'tests/stock/test',env=dict(os.environ,**overrides),capture_output=True,text=True,timeout=120)
 (r/('stock'+mode+'.stdout')).write_text(p.stdout);(r/('stock'+mode+'.stderr')).write_text(p.stderr)
 rows.append({'mode':mode,'command':cmd,'cwd':str(tree/'tests/stock/test'),'env_override':overrides,'exit':p.returncode,'seconds':time.monotonic()-start,'stdout':p.stdout,'stderr':p.stderr})
 (r/'stock-results.json').write_text(json.dumps(rows,indent=2)+'\n'); print(json.dumps(rows[-1]),flush=True)
