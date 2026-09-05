from pathlib import Path
import subprocess,os,json
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');res=[]
for v in ['fix-normal','fix-assert']:
 tree=r/v;exe=r/(v+'-flag-error')
 cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(r/'flag-error.c'),str(tree/'src/libluajit.a'),'-Wl,--wrap=lj_mem_realloc','-lm','-ldl','-pthread','-o',str(exe)]
 if v=='fix-assert':cmd[1:1]=['-DLUA_USE_ASSERT','-DLJ_GC2_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS']
 p=subprocess.run(cmd,capture_output=True,text=True);assert p.returncode==0,p.stderr
 env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;'
 run=['taskset','-c','0-15',str(exe)];p=subprocess.run(run,capture_output=True,text=True,env=env,timeout=20)
 for k in ['stdout','stderr']:(r/(v+'-flag-error.'+k)).write_text(getattr(p,k))
 res.append(dict(variant=v,compile=cmd,command=run,exit=p.returncode,stdout=p.stdout,stderr=p.stderr));print(v,p.returncode,p.stdout,p.stderr)
(r/'flag-error-results.json').write_text(json.dumps(res,indent=2)+'\n')
