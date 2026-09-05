#!/usr/bin/env python3
import datetime,hashlib,itertools,json,os,pathlib,resource,shutil,subprocess,time
P=pathlib.Path(__file__).resolve().parent
ROOT=pathlib.Path('/tmp/lj-worker-bridge-combined-20260905-bz9wysjp')
KINDS=('candidate','strict','asan')
FLAGS=['-DLUA_USE_APICHECK','-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS','-DLJ_UDATA_TEST_HELPERS','-DLJ_STR_TEST_HELPERS']
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def ident(p): return {'sha256':sha(p),'bytes':p.stat().st_size}
def dump(name,x): (P/name).write_text(json.dumps(x,indent=2,sort_keys=True)+'\n')
def verify_sources(expected):
 out={}
 for kind in KINDS:
  records={name:{'expected':h,'actual':sha(ROOT/kind/name)} for name,h in expected.items()}
  mismatches={name:r for name,r in records.items() if r['expected']!=r['actual']}
  out[kind]={'checked':len(records),'mismatches':mismatches,'inputs':records}
 return out
setup=json.load(open(ROOT/'setup.json'))
assert len(setup['combined_inputs'])==225
checks=verify_sources(setup['combined_inputs'])
dump('source-checks-before.json',checks)
assert all(x['checked']==225 and not x['mismatches'] for x in checks.values())
(P/'fixtures').mkdir(); (P/'provenance').mkdir()
for name in ('t-string-retention.c','peer-control.lua'):
 shutil.copy2(ROOT/'fixtures'/name,P/'fixtures'/name)
shutil.copy2(ROOT/'setup.json',P/'provenance'/'root-setup.json')
setup_out={'root_package':str(ROOT),'root_setup':ident(ROOT/'setup.json'),'head':setup['head'],'changed_runtime_inputs':setup['changed_runtime_inputs'],'source_inputs_per_tree':225,'fixture_bounds':{'alarm_seconds':45,'external_timeout_seconds':50,'rounds':6,'burst_tables':4096,'max_bursts_per_round':64,'max_tables_per_round':262144,'live_ring':32,'native_waits_per_burst':8,'native_wait_seconds':0.002,'target_completed_cycles_per_round':3},'fixtures':{},'reused_inputs':{},'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
for name in ('t-string-retention.c','peer-control.lua'):
 setup_out['fixtures'][name]=ident(P/'fixtures'/name)
assert setup_out['fixtures']['t-string-retention.c']['sha256']=='2e8e840fb4ba3a3b09168c06d828ff10ebafd41e9ff555b9737f34384fea3cf9'
assert setup_out['fixtures']['peer-control.lua']['sha256']=='519ebf714b0a33b9a436d3452a153a1bc3eea3322ebf0bf74d8e76fea4ab8cb2'
for kind in KINDS:
 build=json.load(open(ROOT/f'{kind}-build.json'))
 assert build['flags']==([] if kind=='candidate' else FLAGS)
 identities=json.load(open(ROOT/f'results-{kind}'/'identities.json'))
 verified={name:{'expected':v,'actual':ident(pathlib.Path(name))} for name,v in identities.items()}
 assert all(v['expected']==v['actual'] for v in verified.values()),kind
 dump(f'{kind}-reused-identity-checks.json',verified)
 out=P/f'results-{kind}';out.mkdir()
 exe=ROOT/f'results-{kind}'/'automatic-retention'
 assert ident(exe)==identities[str(exe)]
 assert ident(ROOT/kind/'src/libluajit.a')==identities[str(ROOT/kind/'src/libluajit.a')]
 shutil.copy2(exe,out/'automatic-retention')
 assert ident(exe)==ident(out/'automatic-retention')
 for old,new in [(ROOT/f'{kind}-build.json',f'{kind}-build.json'),(ROOT/f'results-{kind}'/'identities.json',f'{kind}-original-identities.json'),(ROOT/f'results-{kind}'/'results.json',f'{kind}-original-results.json')]: shutil.copy2(old,P/'provenance'/new)
 original_rows=json.load(open(ROOT/f'results-{kind}'/'results.json'))
 compile_row=next(r for r in original_rows if r['name']=='automatic-retention-compile')
 setup_out['reused_inputs'][kind]={'source_tree':str(ROOT/kind),'build_flags':build['flags'],'archive':{'path':str(ROOT/kind/'src/libluajit.a'),**ident(ROOT/kind/'src/libluajit.a')},'original_executable':{'path':str(exe),**ident(exe)},'copied_executable':{'path':str(out/'automatic-retention'),**ident(out/'automatic-retention')},'verified_original_identities':len(verified),'original_compile':compile_row}
dump('setup.json',setup_out)
(P/'environment.txt').write_text(subprocess.check_output(['uname','-a'],text=True)+subprocess.check_output(['cc','--version'],text=True)+subprocess.check_output(['clang','--version'],text=True)+'affinity='+str(sorted(os.sched_getaffinity(0)))+'\n')
print('IDENTITIES PASS: 225 inputs x 3 trees; exact archives, fixtures, executable and original dependency identities; strict/ASan exact ten flags; default no helpers',flush=True)
rows=[]
for kind in KINDS:
 out=P/f'results-{kind}'
 original=json.load(open(ROOT/f'results-{kind}'/'results.json'))
 for index,(peer,workers) in enumerate(itertools.product((0,1),(0,2))):
  old=next(r for r in original if r['name']==f'automatic-retention-{index}')
  env=os.environ.copy();env.pop('ASAN_OPTIONS',None)
  controlled=old['environment'].copy();controlled['RETENTION_JIT']='1'
  assert (kind!='asan' or controlled['ASAN_OPTIONS']=='detect_leaks=1:abort_on_error=1')
  env.update(controlled)
  command=['taskset','-c','0-15',str(out/'automatic-retention'),'0',str(peer),str(workers),str(P/'fixtures/peer-control.lua')]
  name=f'jit-peer{peer}-workers{workers}'
  row={'name':name,'kind':kind,'peer':peer,'workers':workers,'command':command,'cwd':old['cwd'],'environment':controlled,'removed_environment':[] if kind=='asan' else ['ASAN_OPTIONS'],'timeout_seconds':50,'internal_alarm_seconds':45,'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'stdout':str((out/f'{name}.stdout').relative_to(P)),'stderr':str((out/f'{name}.stderr').relative_to(P))}
  start=time.monotonic()
  with open(P/row['stdout'],'wb') as fo,open(P/row['stderr'],'wb') as fe:
   try:
    r=subprocess.run(command,cwd=row['cwd'],env=env,stdout=fo,stderr=fe,timeout=50)
    row.update(exit=r.returncode,timed_out=False)
   except subprocess.TimeoutExpired:
    row.update(exit=None,timed_out=True)
  row['seconds']=time.monotonic()-start
  lines=(P/row['stdout']).read_text(errors='replace').splitlines()
  records=[json.loads(x) for x in lines if x.startswith('{')]
  snapshots=[r for r in records if 'stage' in r]
  execution=[r for r in records if 'execution' in r]
  row['summary']={'terminal_lines':[x for x in lines if not x.startswith('{')],'stages':[r['stage'] for r in snapshots],'settled_completed':[r['completed'] for r in snapshots if r['stage']=='settled'],'jit_enabled_values':sorted(set(r['jit_enabled'] for r in execution)),'jit_hard_checks_max':max([r['jit_hard_checks'] for r in execution],default=0),'last_snapshot':snapshots[-1] if snapshots else None,'stderr_bytes':(P/row['stderr']).stat().st_size}
  rows.append(row);dump('results.json',rows)
  print(kind,name,'exit',row['exit'],'timeout',row['timed_out'],'seconds',round(row['seconds'],3),'jit_checks',row['summary']['jit_hard_checks_max'],'tail',row['summary']['terminal_lines'][-1:] ,flush=True)
checks=verify_sources(setup['combined_inputs']);dump('source-checks-after.json',checks)
assert all(x['checked']==225 and not x['mismatches'] for x in checks.values())
dump('summary.json',{'cases':len(rows),'passed':sum(r['exit']==0 and not r['timed_out'] for r in rows),'failed':[{'kind':r['kind'],'name':r['name'],'exit':r['exit'],'timed_out':r['timed_out']} for r in rows if r['exit']!=0 or r['timed_out']],'all_jit_enabled':all(r['summary']['jit_enabled_values']==[1] for r in rows),'source_checks_before_after':{'trees':3,'inputs_per_tree':225,'mismatches':0}})
print('MATRIX',json.dumps(json.load(open(P/'summary.json'))),flush=True)
