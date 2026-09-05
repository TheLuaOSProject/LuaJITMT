from pathlib import Path
import hashlib,json,os,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;q=p/'validation';kind=sys.argv[1];rows=[]
for runner,suites in [('run-final.py',['compile','full','gc','wrapper','gc-wrapper']),('run-supplement.py',['compile','full']),('run-probes.py',['compile','full']),('run-retention.py',['compile','full'])]:
 for suite in suites:
  cmd=['taskset','-c','0-15','python3',str(q/runner),kind,suite]
  start=time.monotonic();r=subprocess.run(cmd,cwd=q,capture_output=True,text=True,timeout=600)
  row={'command':cmd,'seconds':time.monotonic()-start,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
  rows.append(row);(p/(kind+'-matrix-driver.json')).write_text(json.dumps(rows,indent=2)+'\n')
  print(kind,runner,suite,r.returncode,flush=True);assert r.returncode==0,row
prefixes=['final-'+kind+'-','supplement-'+kind+'-','probe-v3-'+kind+'-','retention-v1-'+kind+'-']
functional=[]
for f in q.glob('*results.json'):
 if any(f.name.startswith(x) for x in prefixes):
  rr=json.loads(f.read_text());assert all(r['exit']==0 for r in rr),f
  if '-compile-' not in f.name:functional+=rr
expected=190 if kind=='candidate' else 202
assert len(functional)==expected,(kind,len(functional),expected)
print(kind,'scoped passes',len(functional),flush=True)
