from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0));p=Path(__file__).resolve().parent;kind=sys.argv[1];tree=p/kind
rows=[];env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;'
if kind=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
else:env.pop('ASAN_OPTIONS',None)
def run(label,args,positive=True):
 cmd=['taskset','-c','24-27',str(tree/'src/luajit')]+args;start=time.monotonic()
 try:
  r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=25)
  data={'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
 except subprocess.TimeoutExpired as e:data={'exit':None,'timeout':True,'stdout':(e.stdout or b'').decode(errors='replace'),'stderr':(e.stderr or b'').decode(errors='replace')}
 rows.append({'label':label,'command':cmd,'cwd':str(tree),'environment':{k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env},'seconds':time.monotonic()-start,**data})
 (p/(kind+'-validation.json')).write_text(json.dumps(rows,indent=2)+'\n')
 assert data['exit']==0,(label,data)
for fixture in ['original','direct']:
 modes=['function','table','missing','nonfunction','resize','replace','replace_missing','table_entry','methodlife']
 if fixture=='original':modes+=['newindex','newindex_table']
 path=p/('t-jit-special-udata-guards.lua' if fixture=='original' else 't-special-udata-pure.lua')
 for obj in ['clib','file','buffer','plain']:
  for mode in modes:
   for status in ['-joff','-jon']:
    run('/'.join([fixture,obj,mode,status]),[status,str(path),obj,mode]+(['baseline'] if kind=='guarded' and fixture=='direct' else []))
 print(kind,fixture,'complete',flush=True)
# Existing same-flag, unchanged-allowlist native mutation and exclusion controls.
for mode in ['index','newindex','missing','nonfunction','resize','methodlife','replace']:
 run('cdata/'+mode,['-jon',str(tree/'tests/t-jit-cdata-pure.lua'),mode])
for mode in ['allocate','luastore','newref','clear','foreign','indirect','fpmath']:
 run('excluded-cdata/'+mode,['-jon',str(tree/'tests/t-jit-cdata-pure-exclusions.lua'),mode])
run('side-cdata',['-jon',str(tree/'tests/t-jit-cdata-pure-side.lua')])
print(kind,'all',len(rows),'passed',flush=True)
