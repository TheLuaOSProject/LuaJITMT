from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0));p=Path(__file__).resolve().parent;kind=sys.argv[1];tree=p/kind
strict=kind in ['strict','asan'];asan=kind=='asan';cc='clang' if asan else 'cc'
flags=['-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS'] if strict else []
san=['-fsanitize=address','-fno-omit-frame-pointer'] if asan else []
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;'
if asan:env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
else:env.pop('ASAN_OPTIONS',None)
rows=[];binaries={};exe=tree/'src/luajit'
def sha(f):return hashlib.sha256(f.read_bytes()).hexdigest()
def run(label,cmd,inputs=[],cwd=tree,bound=30,test=True):
 start=time.monotonic()
 try:
  r=subprocess.run(cmd,cwd=cwd,env=env,capture_output=True,text=True,timeout=bound);data={'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
 except subprocess.TimeoutExpired as e:data={'exit':None,'timeout':True,'stdout':(e.stdout or b'').decode(errors='replace'),'stderr':(e.stderr or b'').decode(errors='replace')}
 rows.append({'label':label,'command':cmd,'cwd':str(cwd),'environment':{k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env},'test':test,'inputs':{str(f):sha(f) for f in inputs},'seconds':time.monotonic()-start,**data})
 (p/(kind+'-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
 assert data['exit']==0,(label,data)
 return data
if asan:
 assert '__asan_' not in run('host-instrumentation',['nm',str(tree/'src/host/minilua'),str(tree/'src/host/buildvm')],test=False)['stdout']
 assert '__asan_' in run('runtime-instrumentation',['nm',str(tree/'src/lj_opt_mem.o'),str(tree/'src/lj_crecord.o')],test=False)['stdout']
for status in ['-joff','-jon']:
 run('stock/'+status,['taskset','-c','24-27',str(exe),status,'test.lua','--quiet'],inputs=[exe,tree/'tests/stock/test/test.lua'],cwd=tree/'tests/stock/test',bound=90)
print(kind,'stock complete',flush=True)
libs=[]
for value in [11,29]:
 lib=p/(kind+'-receiver-'+str(value)+'.so');src=p/'t-ffi-clib-receiver-lib.c'
 run('compile-library/'+str(value),[cc,'-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-DNAMESPACE_VALUE='+str(value),str(src),'-o',str(lib)],inputs=[src],test=False)
 libs.append(lib);binaries[lib.name]=sha(lib)
for mode in ['index-other','index-type','newindex-other','newindex-type','index-life','newindex-life','index-side-other','index-side-type','newindex-side-other','newindex-side-type']:
 for status in ['-joff','-jon']:
  src=p/'t-ffi-clib-receiver.lua'
  run('captured/'+mode+'/'+status,['taskset','-c','24-27',str(exe),status,str(src),mode]+list(map(str,libs)),inputs=[exe,src]+libs)
print(kind,'captured receiver complete',flush=True)
for obj in ['clib','file','buffer','plain']:
 for mode in ['function','table','missing','nonfunction','resize','replace','replace_missing','table_entry','methodlife']:
  src=p/'t-special-udata-pure.lua'
  run('direct/'+obj+'/'+mode,['taskset','-c','24-27',str(exe),'-jon',str(src),obj,mode],inputs=[exe,src])
for mode in ['luastore','newref','allocate','indirect','directstore','clear','foreign','fpmath']:
 src=p/'t-udata-pure-exclusions.lua'
 run('udata-effect/'+mode,['taskset','-c','24-27',str(exe),'-jon',str(src),mode],inputs=[exe,src])
print(kind,'direct userdata complete',flush=True)
for mode in ['index','newindex','missing','nonfunction','resize','methodlife','replace']:
 src=tree/'tests/t-jit-cdata-pure.lua'
 run('cdata/'+mode,['taskset','-c','24-27',str(exe),'-jon',str(src),mode],inputs=[exe,src])
for mode in ['allocate','luastore','newref','clear','foreign','indirect','fpmath']:
 src=tree/'tests/t-jit-cdata-pure-exclusions.lua'
 run('cdata-effect/'+mode,['taskset','-c','24-27',str(exe),'-jon',str(src),mode],inputs=[exe,src])
for what in ['side','profile']:
 src=tree/('tests/t-jit-cdata-pure-'+what+'.lua')
 run('cdata/'+what,['taskset','-c','24-27',str(exe),'-jon',str(src)],inputs=[exe,src])
for fixture in ['phase','error','first-attach']:
 src=tree/('tests/t-jit-'+('cdata-pure-' if fixture!='first-attach' else '')+fixture+'.c');out=p/(kind+'-'+fixture)
 cmd=[cc,'-std=gnu11','-O1' if asan else '-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+san+['-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(src),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(out)]
 if fixture=='error':cmd+=['-Wl,--wrap=lj_mem_realloc']
 run('compile-'+fixture,cmd,inputs=[src,tree/'src/libluajit.a'],test=False);binaries[out.name]=sha(out)
 for mode in {'phase':['gate','worker'],'error':[None],'first-attach':[None,'noloop']}[fixture]:
  run(fixture+'/'+str(mode),['taskset','-c','24-27',str(out)]+([mode] if mode else []),inputs=[out])
(p/(kind+'-fixture-binaries.json')).write_text(json.dumps(binaries,indent=2)+'\n')
print(kind,'all',len(rows),'commands,',sum(row['test'] for row in rows),'runtime processes passed',flush=True)
