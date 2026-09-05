from pathlib import Path
import json,os,resource,subprocess,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0));p=Path(__file__).resolve().parent;rows=[]
def run(variant,label,args,expect=0,needle=None):
 tree=p/variant;env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;'
 if variant=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
 else:env.pop('ASAN_OPTIONS',None)
 cmd=['taskset','-c','24-27',str(tree/'src/luajit'),'-jon']+args;start=time.monotonic();r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=25)
 rows.append({'variant':variant,'label':label,'command':cmd,'cwd':str(tree),'environment':{k:env[k] for k in ['LUA_PATH','ASAN_OPTIONS'] if k in env},'seconds':time.monotonic()-start,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr,'expected_exit':expect,'expected_error':needle})
 (p/'negative-controls.json').write_text(json.dumps(rows,indent=2)+'\n');assert r.returncode==expect,rows[-1]
 if needle:assert needle in r.stderr,rows[-1]
 print(variant,label,'expected',expect,flush=True)
run('guarded','hoist-witness-disabled',[str(p/'t-special-udata-pure.lua'),'clib','function'],1,'pure metatable reuse shape')
s=(p/'t-special-udata-pure.lua').read_text().replace('  local ok, result = pcall(run, obj,n)','  jit.off(run, true) -- Negative control: preserve warmed trace but bypass native reentry.\n  local ok, result = pcall(run, obj,n)')
(p/'t-special-udata-pure-no-native.lua').write_text(s)
run('candidate','native-witness-disabled',[str(p/'t-special-udata-pure-no-native.lua'),'clib','function'],1,'post-mutation execution must enter the original native loop')
for variant in ['candidate','strict','asan']:
 for mode in ['luastore','newref','allocate','indirect','directstore','clear','foreign','fpmath']:
  run(variant,'udata-effect/'+mode,[str(p/'t-udata-pure-exclusions.lua'),mode])
