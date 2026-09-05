from pathlib import Path
import subprocess,os,json
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');results=[]
for v in ['base-normal','fix-normal','fix-assert']:
 exe=r/(v+'-callback-control');tree=r/v
 cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(r/'callback-control.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if v=='fix-assert':cmd[1:1]=['-DLUA_USE_ASSERT','-DLJ_GC2_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS']
 z=subprocess.run(cmd,capture_output=True,text=True)
 if z.returncode:print(v,z.stderr);continue
 for mode in ['interp','native']:
  env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;';env['DUMP']=str(r/(v+'-callback-'+mode+'.ir'))
  run=['taskset','-c','0-15',str(exe)]+(['interp'] if mode=='interp' else [])
  z=subprocess.run(run,capture_output=True,text=True,env=env,timeout=20)
  for k in ['stdout','stderr']:(r/(v+'-callback-control-'+mode+'.'+k)).write_text(getattr(z,k))
  results.append(dict(variant=v,mode=mode,compile=cmd,command=run,exit=z.returncode,stdout=z.stdout,stderr=z.stderr));print(v,mode,z.returncode,z.stdout,z.stderr[:800])
(r/'callback-control-results.json').write_text(json.dumps(results,indent=2)+'\n')
