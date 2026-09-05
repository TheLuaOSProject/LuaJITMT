from pathlib import Path
import subprocess, os, json, time, hashlib
out=Path(__file__).parent
fixture=Path('/workspaces/lj-lockless/tests/t-gc2-sweep-table-coalescing.c')
results=[]
configs=[('integrated',Path('/tmp/lj-gc-coalescing-final-20260905-0eig4rlf/assert'),'gcc',['-O2'],['-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS']),('protocol',out/'fixed','gcc',['-O2'],[]),('asan',out/'asan','clang',['-O1','-g','-fno-omit-frame-pointer','-fsanitize=address'],['-DLJ_TAB_TEST_HELPERS'])]
for name,tree,cc,opts,extra in configs:
 exe=out/('scheduled-'+name+'-fixture')
 cmd=[cc,'-std=gnu11',*opts,'-Wall','-Wextra','-Werror','-mcx16','-DLJ_GC2_TEST_HELPERS','-DLUA_USE_ASSERT',*extra,'-I'+str(tree/'src'),str(fixture),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 subprocess.run(cmd,check=True)
 env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
 for mode,count in [('all',20),('mark_barrier_saturation',100 if name=='integrated' else 20)]:
  for repeat in range(count):
   t=time.monotonic();r=subprocess.run(['taskset','-c','0-15',str(exe),mode],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env,timeout=40)
   row={'tree':name,'mode':mode,'repeat':repeat,'rc':r.returncode,'elapsed':time.monotonic()-t,'output':r.stdout};results.append(row)
   (out/'scheduled-fixture-results.json').write_text(json.dumps({'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),'results':results},indent=2))
   if r.returncode:raise RuntimeError(row)
  print(name,mode,count,'passed',flush=True)
