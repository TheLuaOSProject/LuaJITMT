from pathlib import Path
import subprocess,time,json,hashlib,os,concurrent.futures,signal
r=Path(__file__).resolve().parent;rows=[]
def run(name,tree,args,cwd=None,timeout=40):
 exe=tree/'src/luajit';cmd=['taskset','-c','0-15',str(exe)]+args;env=os.environ.copy();env['LUA_PATH']=str(tree/'tests/lib/?.lua')+';'+str(tree/'src/?.lua')+';;';cwd=cwd or tree
 t=time.monotonic();p=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=p.communicate(timeout=timeout);status='complete'
 except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);out,err=p.communicate();status='timeout'
 (r/(name+'.stdout')).write_text(out);(r/(name+'.stderr')).write_text(err)
 result={'name':name,'command':cmd,'cwd':str(cwd),'environment_overrides':{'LUA_PATH':env['LUA_PATH']},'exit':p.returncode,'status':status,'seconds':time.monotonic()-t,'runtime_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'stdout':name+'.stdout','stderr':name+'.stderr'}
 print(name,p.returncode,round(result['seconds'],3),flush=True);return result
jobs=[]
for variant in ('base-normal','fix-normal'):
 tree=r/variant
 for mode in ('joff','jon'):
  jobs.append(('stock-'+variant+'-'+mode,tree,['-'+mode,'test.lua'],tree/'tests/stock/test',40))
for variant in ('fix-normal','fix-assert'):
 tree=r/variant
 for fixture in ('t-ffi-cdata-get-l.lua','t-ffi-cdata-set-l.lua','t-ffi-gc-trace.lua'):
  jobs.append(('ffi-'+variant+'-'+fixture,tree,['-jon',str(tree/'tests'/fixture)],None,30))
 for mode in ('replace','reentrant','inplace'):
  jobs.append(('original-v2-'+variant+'-'+mode,tree,['-jon',str(r/'basemt-native.lua'),mode],None,20))
 jobs.append(('all-guards-'+variant,tree,['-jon',str(r/'t-jit-cdata-basemt-guards.lua')],None,20))
jobs.append(('hammer-fix-normal',r/'fix-normal',['-jon',str(r/'fix-normal/tests/t-ffi-cdata-shared-hammer.lua')],None,30))
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:rows=list(ex.map(lambda j:run(*j),jobs))
(r/'guard-regression-results.json').write_text(json.dumps(rows,indent=2)+'\n')
