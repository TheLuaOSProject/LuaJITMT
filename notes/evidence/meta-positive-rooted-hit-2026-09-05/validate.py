from pathlib import Path
import os,subprocess,time,signal,json
p=Path(__file__).parent;records=[]
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
def run(label,cmd,cwd,limit=45,extra=None,expected_negative=False):
 env=os.environ.copy();env['LUA_PATH']=str(cwd/'src/?.lua')+';'+str(cwd/'tests/lib/?.lua')+';;'
 if extra:env.update(extra)
 start=time.monotonic()
 with (p/(label+'.stdout')).open('w') as out,(p/(label+'.stderr')).open('w') as err:
  proc=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=out,stderr=err,start_new_session=True)
  try:proc.wait(timeout=limit);status='complete'
  except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait();status='timeout'
 stderr=(p/(label+'.stderr')).read_text();stdout=(p/(label+'.stdout')).read_text()
 ok=(status=='complete' and ((proc.returncode!=0 and 'bounded_hit' in stderr and 'outword' in stderr) if expected_negative else proc.returncode==0))
 r={'label':label,'command':cmd,'cwd':str(cwd),'environment':{'LUA_PATH':env['LUA_PATH'],**(extra or {})},'seconds':time.monotonic()-start,'limit_seconds':limit,'status':status,'exit':proc.returncode,'expected_negative':expected_negative,'expected_result':ok,'stdout':stdout,'stderr':stderr};records.append(r);(p/'initial-validation.json').write_text(json.dumps(records,indent=2)+'\n');print(label,proc.returncode,ok,flush=True)
 return ok
for kind in ['assert','asan','negative']:
 root=p/kind
 for test in (['t-tab-rooted-get-try'] if kind=='negative' else ['t-tab-rooted-get-try','t-tab-scalar-hit','t-tab-rooted-len-try','t-tab-rooted-reader']):
  compiler=['clang','-O1','-g','-fno-omit-frame-pointer','-fsanitize=address'] if kind=='asan' else ['gcc','-O2','-g']
  cmd=compiler+['-std=gnu11','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(root/'src'),str(root/'tests'/ (test+'.c')),str(root/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(p/(kind+'-'+test))]
  r=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=40)
  (p/(kind+'-'+test+'-compile.json')).write_text(json.dumps({'command':cmd,'exit':r.returncode,'output':r.stdout},indent=2)+'\n')
  if r.returncode:print(r.stdout,flush=True);continue
  run(kind+'-'+test,['taskset','-c','0-15',str(p/(kind+'-'+test))],root,extra={'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1'} if kind=='asan' else None,expected_negative=kind=='negative')
root=p/'normal'
for mode in ['-joff','-jon']:
 run('stock-'+mode[1:],['taskset','-c','0-15',str(root/'src/luajit'),mode,'test.lua'],root/'tests/stock/test',60)
for label,mode,test,env in [('resize-general','-joff','t-tab-resize-stress.lua',{'LJ_TAB_RESIZE_CASES':'array,hash,stringkey,boolean,delete,weak,metatable'}),('resize-jit','-jon','t-tab-resize-stress.lua',{'LJ_TAB_RESIZE_CASES':'jitstore,jitread,jititer'}),('resize-weakfin','-jon','t-tab-resize-stress.lua',{'LJ_TAB_RESIZE_CASES':'weakfinjit'}),('rooted-readers','-jon','t-tab-rooted-readers.lua',{})]:
 if not (root/'tests'/test).exists():print('missing',test,flush=True);continue
 run(label,['taskset','-c','0-15',str(root/'src/luajit'),mode,str(root/'tests'/test)],root,60,env)
print('expected',sum(r['expected_result'] for r in records),'of',len(records),flush=True)
