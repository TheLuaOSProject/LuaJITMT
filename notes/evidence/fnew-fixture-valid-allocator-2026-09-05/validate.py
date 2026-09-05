from pathlib import Path
import subprocess,json,time,os,signal,hashlib
r=Path(__file__).parent;rows=[]
def run(name,cmd,env=None,cwd=None,timeout=50):
 start=time.monotonic();p=subprocess.Popen(cmd,env=env,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:o,e=p.communicate(timeout=timeout);status='complete'
 except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);o,e=p.communicate();status='timeout'
 (r/(name+'.stdout')).write_text(o);(r/(name+'.stderr')).write_text(e)
 row={'name':name,'command':cmd,'env':{k:env[k] for k in ['LJ_TEST_ROOT','TMPDIR','JOBS','LJ_TEST_DISABLE_BUILD_CACHE'] if env and k in env},'cwd':str(cwd) if cwd else None,'rc':p.returncode,'status':status,'elapsed':time.monotonic()-start,'stdout':name+'.stdout','stderr':name+'.stderr'};rows.append(row);(r/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(row,flush=True);return row
base_runner='/tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/baseline-strict/src/luajit'
env=os.environ.copy();env.update({'LJ_TEST_ROOT':str(r/'canonical'),'TMPDIR':str(r/'tmp'),'JOBS':'4','LJ_TEST_DISABLE_BUILD_CACHE':'1'})
run('canonical',['taskset','-c','0-15',base_runner,str(r/'canonical/tools/test.lua'),'m6_jit_fnew_bump'],env=env,cwd=r/'canonical',timeout=55)
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLUA_USE_ASSERT'];t=r/'strict'
if run('strict-build',['taskset','-c','0-15','make','-C',str(t/'src'),'-j4','BUILDMODE=static','XCFLAGS='+' '.join(flags)])['rc']==0:
 for name,src in [('strict-fixture',t/'tests/t-jit-fnew-bump.c'),('strict-forged-control',r/'forged-identity-control.c')]:
  ex=r/name;cmd=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(t/'src'),'-I'+str(t/'tests'),str(src),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(ex)]
  if run(name+'-compile',cmd)['rc']==0:run(name,['taskset','-c','0-15',str(ex)],timeout=25)
# The helper is absent when its build flag is absent. Compare preprocessed normal code.
old=subprocess.check_output(['git','show','28de50a6:src/lj_func.c'],cwd='/workspaces/lj-lockless',text=True)
new=(t/'src/lj_func.c').read_text();pre=[]
for label,source in [('base',old),('repair',new)]:
 q=subprocess.run(['gcc','-E','-P','-x','c','-','-I'+str(t/'src')],input=source,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,cwd=t/'src')
 (r/('normal-preprocessed-'+label+'.c')).write_text(q.stdout)
 pre.append({'label':label,'rc':q.returncode,'sha256':hashlib.sha256(q.stdout.encode()).hexdigest(),'stderr':q.stderr})
(r/'normal-preprocessor-check.json').write_text(json.dumps({'command':['gcc','-E','-P','-x','c','-','-I'+str(t/'src')],'results':pre,'identical':pre[0]['sha256']==pre[1]['sha256']},indent=2)+'\n')
print('normal preprocessed code identical',pre[0]['sha256']==pre[1]['sha256'],flush=True)
