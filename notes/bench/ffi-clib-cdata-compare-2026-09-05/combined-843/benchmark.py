from pathlib import Path
import hashlib,json,os,resource,subprocess,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
trees={'baseline':Path('/tmp/lj-gc-auto-control-root-20260905-au933s3w/candidate'),
       'candidate':p.parent/'candidate'}
helpers={'baseline':'lj_tab_gettv_rooted_hit_try','candidate':'lj_tab_cmpcdata_kgc_rooted_try'}
out=p/'cost-results.json';assert not out.exists();rows=[]
for pair in range(7):
 for variant in (['baseline','candidate'] if pair%2==0 else ['candidate','baseline']):
  tree=trees[variant];fixture=p/'shared-cost.lua'
  envadd={'LUA_PATH':str(tree/'src/?.lua')+';;','BENCH_SCALE':'1'}
  cmd=['taskset','-c','31',str(tree/'src/luajit'),'-jon',str(fixture),'2000000',helpers[variant]]
  start=time.monotonic()
  try:
   q=subprocess.run(cmd,cwd=p,env={**os.environ,**envadd},capture_output=True,text=True,timeout=45)
   result={'exit':q.returncode,'stdout':q.stdout,'stderr':q.stderr}
  except subprocess.TimeoutExpired as e:
   dec=lambda v:v.decode(errors='replace') if isinstance(v,bytes) else v or ''
   result={'exit':None,'timeout':True,'stdout':dec(e.stdout),'stderr':dec(e.stderr)}
  rows.append({'workload':'shared','pair':pair,'variant':variant,'command':cmd,'cwd':str(p),
               'environment':envadd,'seconds':time.monotonic()-start,
               'inputs':{str(f):sha(f) for f in [fixture,p/'benchmark.py',tree/'src/luajit',tree/'src/libluajit.a',tree/'src/jit/vmdef.lua']},**result})
  out.write_text(json.dumps(rows,indent=2)+'\n')
  print(pair,variant,'exit',result['exit'],result['stdout'].strip().splitlines()[-1:] or result['stderr'],flush=True)
  assert result['exit']==0,rows[-1]
