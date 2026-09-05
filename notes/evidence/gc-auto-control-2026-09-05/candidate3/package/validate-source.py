from pathlib import Path
import json, hashlib, difflib
r=Path(__file__).resolve().parent
old=Path('/tmp/lj-gc-auto-control-20260905-qs673ryl')
base=json.loads((r/'source-identity.json').read_text())['files']
want=json.loads((r/'candidate3-source.json').read_text())['files']
rows={}
for v in ['candidate3','strict','asan']:
 changes=[]
 for name,bsha in base.items():
  q=r/v/name
  assert q.exists(), (v,name,'missing')
  sha=hashlib.sha256(q.read_bytes()).hexdigest()
  assert sha==want.get(name,bsha), (v,name,'unexpected source difference')
  if sha!=bsha:changes.append(dict(path=name,sha256=sha))
 rows[v]=dict(tracked_inputs=len(base),changed=changes)
fixtures=['t-stop-first-attach.c','t-stop-first-attach-restart-v2.c','t-restart-first-attach.c','t-restart-last-detach.c','t-auto-restart-numeric-max.c','t-string-retention.c','t-auto-controls.c','t-auto-controls-v2.c','peer-control.lua','control-boundaries.lua','t-auto-finalizer-controls.c','t-finalizer-spawn-query-overlap.lua']
rows['unchanged_fixtures']={}
for name in fixtures:
 assert (r/name).read_bytes()==(old/name).read_bytes(),name
 rows['unchanged_fixtures'][name]=hashlib.sha256((r/name).read_bytes()).hexdigest()
tests=['t-m8-finalizer-state.c','t-m8-close-finalizers.c','t-ffi-gc-finreg.lua','t-m8-finalizer-spawn-live.lua']
rows['unchanged_existing_finalizers']={}
for name in tests:
 p=r/'candidate3/tests'/name
 assert p.read_bytes()==(old/'candidate2/tests'/name).read_bytes(),name
 rows['unchanged_existing_finalizers'][name]=hashlib.sha256(p.read_bytes()).hexdigest()
for name in ['automatic-query-candidate3.patch','automatic-control-candidate3-full.patch']:
 assert hashlib.sha256((r/name).read_bytes()).hexdigest()==want[name],name
(r/'source-validation.json').write_text(json.dumps(rows,indent=2)+'\n')
delta=[]
for oldname,newname in [('t-auto-finalizer-controls.c','t-auto-finalizer-controls-v3.c'),('t-finalizer-spawn-query-overlap.lua','t-finalizer-spawn-query-enabled.lua')]:
 delta.extend(difflib.unified_diff((r/oldname).read_text().splitlines(keepends=True),(r/newname).read_text().splitlines(keepends=True),fromfile='a/'+oldname,tofile='b/'+newname))
(r/'versioned-query-oracles.patch').write_text(''.join(delta))
print(dict(variants=3,tracked_inputs=len(base),changed_per_variant=6,unchanged_fixtures=len(fixtures),unchanged_existing_finalizers=len(tests)))
