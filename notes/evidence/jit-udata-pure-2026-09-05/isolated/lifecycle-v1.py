from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0));p=Path(__file__).resolve().parent;kind=sys.argv[1];tree=p/kind;rows=[]
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;'
if kind=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
else:env.pop('ASAN_OPTIONS',None)
def run(label,cmd,expected=0):
 start=time.monotonic();r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=30)
 rows.append({'label':label,'command':cmd,'cwd':str(tree),'environment':{k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env},'seconds':time.monotonic()-start,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
 (p/(kind+'-lifecycle-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
 assert r.returncode==expected,(label,rows[-1]);print(kind,label,'passed',flush=True)
 if label=='runtime-instrumentation':assert '__asan_' in r.stdout
 if label=='host-instrumentation':assert '__asan_' not in r.stdout
if kind=='asan':
 run('host-instrumentation',['nm',str(tree/'src/host/minilua'),str(tree/'src/host/buildvm')])
 run('runtime-instrumentation',['nm',str(tree/'src/lj_opt_mem.o')])
for fixture in ['phase','error','first-attach']:
 path=tree/('tests/t-jit-'+('cdata-pure-' if fixture!='first-attach' else '')+fixture+'.c');exe=p/(kind+'-'+fixture)
 cmd=['clang' if kind=='asan' else 'cc','-g','-O1' if kind=='asan' else '-O2','-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(path),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if kind=='asan':cmd+=['-fsanitize=address','-fno-omit-frame-pointer']
 if fixture=='error':cmd+=['-Wl,--wrap=lj_mem_realloc']
 run('compile-'+fixture,cmd)
 for mode in ({'phase':['gate','worker'],'error':[None],'first-attach':['loop','noloop']}[fixture]):
  run(fixture+'/'+str(mode),['taskset','-c','24-27',str(exe)]+([mode] if mode else []))
run('profile',['taskset','-c','24-27',str(tree/'src/luajit'),'-jon',str(tree/'tests/t-jit-cdata-pure-profile.lua')])
