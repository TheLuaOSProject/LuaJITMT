from pathlib import Path
import subprocess,json,os,time
out=Path(__file__).parent;tree=out/'wide-asan';env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=0'
commands=[['make','-C',str(tree/'src'),'clean'],['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4','BUILDMODE=static','CC=clang','CCOPT=-O1 -fno-omit-frame-pointer','CCDEBUG=-g','XCFLAGS=-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLUA_USE_ASSERT -fsanitize=address','LDFLAGS=-fsanitize=address']]
with (out/'build-wide-asan.log').open('w') as log:
 for cmd in commands:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,check=True)
exe=out/'fixture-wide-asan';cmd=['clang','-std=gnu11','-O1','-g','-fno-omit-frame-pointer','-fsanitize=address','-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLUA_USE_ASSERT','-I'+str(tree/'src'),str(out/'t-wide-stamp.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)];subprocess.run(cmd,check=True)
env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1';results=[]
for mode in ['all','existing']:
 t=time.monotonic();r=subprocess.run(['taskset','-c','0-15',str(exe),mode],env=env,capture_output=True,text=True,timeout=60)
 row={'mode':mode,'rc':r.returncode,'elapsed':time.monotonic()-t,'stdout':r.stdout,'stderr':r.stderr};results.append(row);print(row,flush=True)
(out/'asan-results.json').write_text(json.dumps({'build_commands':commands,'compile_command':cmd,'environment':{'ASAN_OPTIONS':env['ASAN_OPTIONS']},'runs':results},indent=2))
