from pathlib import Path
import subprocess,os,json
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');res=[]
for v in ['base-normal','fix-normal','fix-assert']:
 tree=r/v;exe=r/(v+'-global-worker');cc=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(r/'global-worker.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if v=='fix-assert':cc[1:1]=['-DLUA_USE_ASSERT','-DLJ_GC2_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS']
 z=subprocess.run(cc,capture_output=True,text=True);assert z.returncode==0,z.stderr
 env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;';cmd=['taskset','-c','0-15',str(exe)]
 z=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=20)
 for k in ['stdout','stderr']:(r/(v+'-global-worker.'+k)).write_text(getattr(z,k))
 res.append(dict(variant=v,compile=cc,command=cmd,exit=z.returncode,stdout=z.stdout,stderr=z.stderr));print(v,z.returncode,z.stdout,z.stderr,flush=True)
(r/'global-worker-results.json').write_text(json.dumps(res,indent=2)+'\n')
