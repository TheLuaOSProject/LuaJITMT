from pathlib import Path
import hashlib, json, os, resource, subprocess, sys, time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
repo=Path('/workspaces/lj-lockless')
variant=sys.argv[1]
tree=p/variant
flags=['-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS',
       '-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS',
       '-DLJ_XSAVE_TEST_HELPERS'] if variant!='candidate' else []
san=['-fsanitize=address','-fno-omit-frame-pointer'] if variant=='asan' else []
cc='clang' if variant=='asan' else 'cc'
envadd={'LUA_PATH':str(tree/'src/?.lua')+';;'}
if variant=='asan': envadd['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
rows=[]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
def run(label,cmd,inputs,test=False):
    start=time.monotonic()
    try:
        q=subprocess.run(cmd,cwd=repo,env={**os.environ,**envadd},capture_output=True,text=True,timeout=30)
        result={'exit':q.returncode,'stdout':q.stdout,'stderr':q.stderr}
    except subprocess.TimeoutExpired as e:
        result={'exit':None,'timeout':True,'stdout':(e.stdout or b'').decode(errors='replace'),
                'stderr':(e.stderr or b'').decode(errors='replace')}
    rows.append({'label':label,'command':cmd,'cwd':str(repo),'environment':envadd,
                 'seconds':time.monotonic()-start,'test':test,
                 'inputs':{str(f):sha(f) for f in inputs},**result})
    (p/(variant+'-recorder-roots-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
    print(variant,label,result['exit'],result['stdout'],result['stderr'],flush=True)
    assert result['exit']==0,rows[-1]
src=p/'t-clib-recorder-roots.c'
exe=p/(variant+'-recorder-roots')
archive=tree/'src/libluajit.a'
run('compile',[cc,'-std=gnu11','-O1' if variant=='asan' else '-O2','-g','-Wall','-Wextra',
               '-Werror','-mcx16']+flags+san+['-I'+str(tree/'src'),'-I'+str(repo/'tests'),
               str(src),str(archive),'-Wl,--wrap=lj_tg_root_anchor_push',
               '-Wl,--wrap=lj_tab_gettv_rooted_hit_try','-lm','-ldl','-pthread','-o',str(exe)],
    [src,archive])
for mode in ['1','2','3','refuse','hit']:
    run(mode,['taskset','-c','0-15',str(exe),mode],[src,exe],True)
