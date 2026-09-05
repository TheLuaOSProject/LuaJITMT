from pathlib import Path
import subprocess,json,os,time,signal,hashlib
r=Path(__file__).resolve().parent;tree=r/'canonical';tmp=r/'canonical-tmp';tmp.mkdir();rows=[]
env=os.environ.copy();overrides={'LJ_TEST_ROOT':str(tree),'JOBS':'4','TMPDIR':str(tmp),'LJ_TEST_DISABLE_BUILD_CACHE':'1'};env.update(overrides)
for case in ('m6_jit_cdata_basemt_guards','m6_jit_xbar_xpoll','m6_jit_mt_activation_flush','m6_jit_gcworkers_activation_flush'):
 cmd=['taskset','-c','0-15',str(r/'base-normal/src/luajit'),'-joff',str(tree/'tools/test.lua'),case]
 t=time.monotonic();p=subprocess.Popen(cmd,cwd=tree,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=p.communicate(timeout=120);status='complete'
 except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);out,err=p.communicate();status='timeout'
 (r/(case+'.stdout')).write_text(out);(r/(case+'.stderr')).write_text(err)
 rows.append({'case':case,'command':cmd,'cwd':str(tree),'environment_overrides':overrides,'exit':p.returncode,'status':status,'seconds':time.monotonic()-t,'runtime_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest() if (tree/'src/luajit').exists() else None})
 print(case,p.returncode,round(rows[-1]['seconds'],3),flush=True)
(r/'canonical-guards-results.json').write_text(json.dumps(rows,indent=2)+'\n')
