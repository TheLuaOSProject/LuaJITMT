from pathlib import Path
import subprocess,json,os,time
out=Path(__file__).parent;results=[]
def run(name,cmd,cwd=None,env=None,timeout=70):
 t=time.monotonic();r=subprocess.run(cmd,cwd=cwd,env=env,capture_output=True,text=True,timeout=timeout)
 row={'name':name,'command':cmd,'cwd':str(cwd) if cwd else None,'rc':r.returncode,'elapsed':time.monotonic()-t,'stdout':r.stdout,'stderr':r.stderr};results.append(row);(out/'validation-results.json').write_text(json.dumps(results,indent=2));print(name,r.returncode,flush=True);return r
for kind in ['base','wide']:
 tree=out/kind;env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;'
 for mode in ['-joff','-jon']:
  run('stock-'+kind+'-'+mode[1:],['taskset','-c','0-15',str(tree/'src/luajit'),mode,'test.lua','--quiet'],tree/'tests/stock/test',env)
tree=out/'wide-helper'
for name,source in [('traverse',out/'traverse-adapter.c'),('recovery',tree/'tests/t-gc2-recovery.c'),('guard',tree/'tests/t-gc2-table-store-guard.c')]:
 exe=out/('related-'+name)
 cmd=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLUA_USE_ASSERT','-I'+str(tree/'src'),str(source),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 r=run('compile-'+name,cmd)
 if not r.returncode:run(name,['taskset','-c','0-15',str(exe)])
