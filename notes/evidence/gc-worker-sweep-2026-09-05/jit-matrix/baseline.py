#!/usr/bin/env python3
import datetime,hashlib,json,os,pathlib,resource,shutil,subprocess,time
P=pathlib.Path(__file__).resolve().parent
ROOT=pathlib.Path('/tmp/lj-worker-bridge-combined-20260905-bz9wysjp')
OWNER=pathlib.Path('/tmp/lj-gc-pending-root-design-20260905-blju2qsh')
TREE=OWNER/'baseline'
OUT=P/'results-baseline';OUT.mkdir()
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def ident(p): return {'sha256':sha(p),'bytes':p.stat().st_size}
def dump(name,x): (P/name).write_text(json.dumps(x,indent=2,sort_keys=True)+'\n')
expected=json.load(open(ROOT/'setup.json'))['before_inputs']
checks={n:{'expected':h,'actual':sha(TREE/n)} for n,h in expected.items()}
assert len(checks)==225 and all(v['expected']==v['actual'] for v in checks.values())
dump('baseline-source-checks-before.json',checks)
build=json.load(open(OWNER/'baseline-build.json'))
assert build['argv']==['make','-C','src','-j2']
archive=TREE/'src/libluajit.a'
assert ident(archive)==build['binaries']['libluajit.a']
assert sha(archive)=='cbc7e955549f291850dd5693dce77ce1d1f56461ced87eacc77e069603880343'
shutil.copy2(OWNER/'baseline-build.json',P/'provenance/eb8-baseline-build.json')
source=P/'fixtures/t-string-retention.c';peerfile=P/'fixtures/peer-control.lua'
assert sha(source)=='2e8e840fb4ba3a3b09168c06d828ff10ebafd41e9ff555b9737f34384fea3cf9'
assert sha(peerfile)=='519ebf714b0a33b9a436d3452a153a1bc3eea3322ebf0bf74d8e76fea4ab8cb2'
exe=OUT/'automatic-retention'
cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-MMD','-MF',str(OUT/'automatic-retention.d'),'-I'+str(TREE/'src'),'-I'+str(TREE/'tests'),str(source),str(archive),'-Wl,-E','-lm','-ldl','-pthread','-o',str(exe)]
env=os.environ.copy();env.pop('ASAN_OPTIONS',None)
controlled={'LUA_PATH':str(TREE/'src/?.lua')+';'+str(TREE/'tests/lib/?.lua')+';;','RETENTION_JIT':'1'}
env.update(controlled)
start=time.monotonic()
with open(OUT/'compile.stdout','wb') as fo,open(OUT/'compile.stderr','wb') as fe:
 r=subprocess.run(cmd,cwd=TREE,env=env,stdout=fo,stderr=fe,timeout=60)
compile_row={'command':cmd,'cwd':str(TREE),'environment':controlled,'removed_environment':['ASAN_OPTIONS'],'timeout_seconds':60,'exit':r.returncode,'seconds':time.monotonic()-start,'stdout':'results-baseline/compile.stdout','stderr':'results-baseline/compile.stderr'}
dump('baseline-compile.json',compile_row);assert r.returncode==0
inputs={str(x):ident(x) for x in (source,peerfile,archive,exe)}
deps=(OUT/'automatic-retention.d').read_text().replace('\\\n',' ').split(':',1)[1].split()
for x in deps: inputs[str(pathlib.Path(x))]=ident(pathlib.Path(x))
dump('baseline-identities.json',{'runtime_commit':'eb8a5b2f9ce2fd6128f4dbeef25b03896b81cfcd','tree':str(TREE),'matched_root_before_inputs':225,'flags':[],'inputs':inputs})
rows=[]
for peer in (0,1):
 name=f'jit-peer{peer}-workers0';cmd=['taskset','-c','0-15',str(exe),'0',str(peer),'0',str(peerfile)]
 row={'name':name,'kind':'eb8-baseline','peer':peer,'workers':0,'command':cmd,'cwd':str(TREE),'environment':controlled,'removed_environment':['ASAN_OPTIONS'],'timeout_seconds':50,'internal_alarm_seconds':45,'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'stdout':f'results-baseline/{name}.stdout','stderr':f'results-baseline/{name}.stderr'}
 start=time.monotonic()
 with open(P/row['stdout'],'wb') as fo,open(P/row['stderr'],'wb') as fe:
  try:
   r=subprocess.run(cmd,cwd=TREE,env=env,stdout=fo,stderr=fe,timeout=50);row.update(exit=r.returncode,timed_out=False)
  except subprocess.TimeoutExpired: row.update(exit=None,timed_out=True)
 row['seconds']=time.monotonic()-start
 lines=(P/row['stdout']).read_text(errors='replace').splitlines();records=[json.loads(x) for x in lines if x.startswith('{')]
 snapshots=[r for r in records if 'stage' in r];execution=[r for r in records if 'execution' in r]
 row['summary']={'terminal_lines':[x for x in lines if not x.startswith('{')],'stages':[r['stage'] for r in snapshots],'settled_completed':[r['completed'] for r in snapshots if r['stage']=='settled'],'jit_enabled_values':sorted(set(r['jit_enabled'] for r in execution)),'jit_hard_checks_max':max([r['jit_hard_checks'] for r in execution],default=0),'last_snapshot':snapshots[-1] if snapshots else None,'stderr_bytes':(P/row['stderr']).stat().st_size}
 rows.append(row);dump('baseline-results.json',rows)
 print('eb8-baseline',name,'exit',row['exit'],'timeout',row['timed_out'],'seconds',round(row['seconds'],3),'jit_checks',row['summary']['jit_hard_checks_max'],'tail',row['summary']['terminal_lines'][-1:],flush=True)
checks={n:{'expected':h,'actual':sha(TREE/n)} for n,h in expected.items()}
assert len(checks)==225 and all(v['expected']==v['actual'] for v in checks.values())
dump('baseline-source-checks-after.json',checks)
