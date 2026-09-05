from pathlib import Path
import subprocess,json,time
out=Path(__file__).parent;tree=out/'negative-ignore-era'
cmd=['taskset','-c','0-15','make','-C',str(tree/'src'),'-j2','BUILDMODE=static','XCFLAGS=-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLUA_USE_ASSERT']
with (out/'build-negative.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
exe=out/'fixture-ignore-era';cc=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLUA_USE_ASSERT','-I'+str(tree/'src'),str(out/'t-wide-stamp.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)];subprocess.run(cc,check=True)
t=time.monotonic();r=subprocess.run(['taskset','-c','0-15',str(exe),'pause'],capture_output=True,text=True,timeout=50)
row={'build_command':cmd,'compile_command':cc,'command':['taskset','-c','0-15',str(exe),'pause'],'rc':r.returncode,'elapsed':time.monotonic()-t,'stdout':r.stdout,'stderr':r.stderr};(out/'negative-results.json').write_text(json.dumps(row,indent=2));print(row,flush=True)
