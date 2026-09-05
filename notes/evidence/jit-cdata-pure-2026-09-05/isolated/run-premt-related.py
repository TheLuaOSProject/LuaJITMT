from pathlib import Path
import subprocess,os,json,shutil,time,signal,hashlib
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');tree=r/'canonical';shutil.copytree(r/'fix-normal',tree,symlinks=True,dirs_exist_ok=True);tmp=r/'canonical-tmp';tmp.mkdir(exist_ok=True)
env=os.environ.copy();overrides={'LJ_TEST_ROOT':str(tree),'JOBS':'4','TMPDIR':str(tmp),'LJ_TEST_DISABLE_BUILD_CACHE':'1'};env.update(overrides)
cases=['m6_jit_cdata_basemt_guards','m6_jit_xbar_xpoll','m6_jit_mt_activation_flush','m6_jit_gcworkers_activation_flush','m5_jit_table_fload_mutable','m5_jit_href_node_order','m5_jit_hrefk_record_snapshot']
cmd=['taskset','-c','0-15',str(r/'base-normal/src/luajit'),'-joff',str(tree/'tools/test.lua')]+cases
t=time.monotonic();p=subprocess.Popen(cmd,cwd=tree,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
try:out,err=p.communicate(timeout=180);status='complete'
except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);out,err=p.communicate();status='timeout'
(r/'canonical-related.stdout').write_text(out);(r/'canonical-related.stderr').write_text(err)
res=[dict(cases=cases,command=cmd,cwd=str(tree),environment_overrides=overrides,exit=p.returncode,status=status,seconds=time.monotonic()-t)]
print('canonical',p.returncode,res[-1]['seconds'],flush=True)
for v in ['fix-normal','fix-assert']:
 for mode in ['-joff','-jon']:
  env=os.environ.copy();env['LUA_PATH']=str(r/v/'src/?.lua')+';;'
  cmd=['taskset','-c','0-15',str(r/v/'src/luajit'),mode,'test.lua']
  t=time.monotonic();z=subprocess.run(cmd,cwd=r/v/'tests/stock/test',env=env,capture_output=True,text=True,timeout=45)
  for k in ['stdout','stderr']:(r/(v+'-stock-'+mode[2:]+'.'+k)).write_text(getattr(z,k))
  res.append(dict(variant=v,mode=mode,command=cmd,cwd=str(r/v/'tests/stock/test'),environment_overrides={'LUA_PATH':env['LUA_PATH']},exit=z.returncode,seconds=time.monotonic()-t));print(v,mode,z.returncode,flush=True)
(r/'related-results.json').write_text(json.dumps(res,indent=2)+'\n')
