from pathlib import Path
import subprocess,shutil,json,hashlib,re,statistics,platform
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');repo=Path('/workspaces/lj-lockless');base='b4e26564542cb8bfa997a11c6a90e5e0017a2f79'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
# All tracked runtime/dynasm input hashes, plus generated/build products.
paths=subprocess.check_output(['git','ls-tree','-r','--name-only',base,'--','src','dynasm'],cwd=repo,text=True).splitlines()
manifest={'base':base,'source':{},'tracked_source_differences':{},'runtime_build_products':{}}
for p in paths:
 if (r/'base-normal'/p).is_file():
  h={v:sha(r/v/p) for v in ['base-normal','fix-normal','fix-assert','canonical'] if (r/v/p).is_file()}
  manifest['source'][p]=h
  if h.get('base-normal')!=h.get('fix-normal'):manifest['tracked_source_differences'][p]=h
for v in ['base-normal','fix-normal','fix-assert','canonical','negative-flag','negative-classifier']:
 products={}
 for p in sorted((r/v/'src').rglob('*')):
  if p.is_file() and (p.suffix in ['.o','.a','.so'] or p.name in ['luajit','lj_vm.S','lj_bcdef.h','lj_ffdef.h','lj_folddef.h','lj_recdef.h','lj_libdef.h','lj_vmdef.h','buildvm','minilua','vmdef.lua']):products[str(p.relative_to(r/v))]={'sha256':sha(p),'bytes':p.stat().st_size}
 manifest['runtime_build_products'][v]=products
manifest['fixture_executables']={p.name:{'sha256':sha(p),'bytes':p.stat().st_size} for p in r.iterdir() if p.is_file() and p.read_bytes()[:4]==b'\x7fELF'}
(r/'source-binary-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
assert set(manifest['tracked_source_differences'])=={'src/lj_jit.h','src/lj_opt_loop.c','src/lj_opt_mem.c'},manifest['tracked_source_differences'].keys()
# Current candidate patch reconstruction, no shared checkout mutation.
import tempfile
patchtree=Path(tempfile.mkdtemp(prefix='patch-verify-',dir=r))
for p in manifest['tracked_source_differences']:
 (patchtree/p).parent.mkdir(parents=True,exist_ok=True);shutil.copy2(r/'base-normal'/p,patchtree/p)
z=subprocess.run(['git','apply',str(r/'candidate.patch')],cwd=patchtree,capture_output=True,text=True);assert z.returncode==0,z.stderr
check={p:sha(patchtree/p)==sha(r/'fix-normal'/p) for p in manifest['tracked_source_differences']};assert all(check.values())
(r/'patch-verification.json').write_text(json.dumps({'patch_sha256':sha(r/'candidate.patch'),'reconstructed':check},indent=2)+'\n')
# Printed harness values are best-of-five; no invented hidden per-run samples.
x=json.loads((r/'field-cost-results.json').read_text());vals={v:[] for v in ['base-normal','fix-normal']}
for a in x['samples']:
 m=re.search(r'^ffi_struct\s+([\d.]+)\s+([\d.]+)',a['stdout'],re.M);assert m
 vals[a['variant']].append(float(m.group(1)))
med={k:statistics.median(v) for k,v in vals.items()}
summary={'fresh_process_pairs':7,'cpu':31,'iterations':30000000,'statistic':'median of seven fresh-process best-of-five printed CPU times','times_seconds':vals,'medians_seconds':med,'relative_change_percent':100*(med['fix-normal']/med['base-normal']-1),'qualified_scope':'unchanged ffi_struct only; exact b4 normal vs patch508e8012; no callback fix or mode0-poll prototype; host not globally isolated; printed seconds have 4 decimals'}
(r/'field-cost-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
metadata={}
for name,cmd in [('uname',['uname','-a']),('compiler',['cc','--version']),('affinity',['taskset','-pc',str(__import__('os').getpid())]),('cpu',['lscpu'])]:
 z=subprocess.run(cmd,capture_output=True,text=True);metadata[name]={'command':cmd,'exit':z.returncode,'stdout':z.stdout,'stderr':z.stderr}
(r/'environment.json').write_text(json.dumps(metadata,indent=2)+'\n')
# Preserve orchestration scripts alongside raw results.
for f in ['/tmp/create-premt-cdata-hoist.py','/tmp/refine-premt-offset.py','/tmp/finish-premt-offset.py','/tmp/run-premt-semantic.py','/tmp/run-premt-phase.py','/tmp/run-premt-eligibility.py','/tmp/run-premt-flag.py','/tmp/prepare-premt-final-controls.py','/tmp/run-premt-worker.py','/tmp/sample-premt-worker.py','/tmp/run-premt-related.py','/tmp/run-premt-classifier-negative.py','/tmp/run-premt-cost.py','/tmp/package-premt-callback.py','/tmp/package-premt-candidate.py']:
 if Path(f).exists():shutil.copy2(f,r/Path(f).name)
print(json.dumps({'source_files':len(manifest['source']),'differences':list(manifest['tracked_source_differences']),'cost':summary,'patch':sha(r/'candidate.patch')},indent=2))
