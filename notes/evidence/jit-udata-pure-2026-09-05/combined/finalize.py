from pathlib import Path
import hashlib,json,subprocess,shutil
p=Path(__file__).resolve().parent;repo=Path('/workspaces/lj-lockless');old=Path('/tmp/lj-special-udata-pure-20260905-u7z61i10')
def sha(f):return hashlib.sha256(f.read_bytes()).hexdigest()
setup=json.loads((p/'setup.json').read_text());identity=json.loads((p/'runtime-input-identity.json').read_text());results={};binaries={}
assert sha(old/'artifact-manifest.json')==setup['prior_manifest_sha256']
for item in json.loads((old/'artifact-manifest.json').read_text())['artifacts']:
 assert sha(old/item['relative_path'])==item['sha256'],item['relative_path']
for kind in ['candidate','strict','asan']:
 tree=p/kind
 assert {f:sha(tree/f) for f in identity['combined'][kind]}==identity['combined'][kind]
 data=json.loads((p/(kind+'-results.json')).read_text());build=json.loads((p/(kind+'-build.json')).read_text())
 assert build['exit']==0 and all(row['exit']==0 for row in data)
 assert len(data)==(94 if kind=='asan' else 92) and sum(row['test'] for row in data)==87
 for row in data:
  for file,digest in row['inputs'].items():assert sha(Path(file))==digest,file
 if kind=='asan':
  nm=next(row['stdout'] for row in data if row['label']=='runtime-instrumentation')
  for obj in ['lj_opt_mem.o','lj_crecord.o']:
   section=nm.split(str(tree/'src'/obj)+':\n',1)[1].split('\n\n',1)[0]
   assert '__asan_' in section,obj
  assert '__asan_' not in next(row['stdout'] for row in data if row['label']=='host-instrumentation')
 binaries[kind]={file:sha(tree/'src'/file) for file in ['luajit','libluajit.a']}
 assert binaries[kind]==json.loads((p/(kind+'-binaries.json')).read_text())
 results[kind]={'commands':len(data),'runtime_processes':87,'stock':{row['label']:row['stdout'].strip() for row in data if row['label'].startswith('stock/')}}
paths=subprocess.check_output(['git','ls-tree','-r','--name-only',setup['base'],'tests/stock'],cwd=repo,text=True).splitlines()
inputs={f:sha(p/'candidate'/f) for f in paths}
for tree in [p/'strict',p/'asan']:
 assert {f:sha(tree/f) for f in paths}==inputs
for f in ['tests/lib/lua_fixture_helpers.h','tests/t-jit-cdata-pure-phase.c','tests/t-jit-cdata-pure-error.c','tests/t-jit-first-attach.c','tests/t-jit-cdata-pure.lua','tests/t-jit-cdata-pure-side.lua','tests/t-jit-cdata-pure-profile.lua','tests/t-jit-cdata-pure-exclusions.lua']:
 inputs[f]=sha(p/'candidate'/f)
for f in setup['frozen_files']:assert sha(p/f)==setup['frozen_files'][f],f
(p/'test-input-identity.json').write_text(json.dumps(inputs,indent=2)+'\n')
(p/'final-validation.json').write_text(json.dumps({'source_bytes_rechecked':True,'prior_handoff_all_artifacts_unchanged':True,'input_files':224,'source_sha256':{f:identity['combined']['candidate'][f] for f in ['src/lj_crecord.c','src/lj_opt_mem.c']},'variants':results,'binaries':binaries,'runtime_processes':261,'validation_commands':278,'build_commands':3,'ir_probe_processes':4,'failures':[],'timing_repeated':False},indent=2)+'\n')
for f in ['lj_crecord.c','lj_opt_mem.c','lj_opt_loop.c','lj_jit.h']:
 out=p/'combined-source';out.mkdir(exist_ok=True);shutil.copyfile(p/'candidate/src'/f,out/f)
checks=[]
for cmd,cwd in [(['git','apply','--reverse','--check',str(p/'candidate-v2.patch')],p/'candidate')]:
 r=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True);checks.append({'command':cmd,'cwd':str(cwd),'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr});assert r.returncode==0
(p/'patch-checks.json').write_text(json.dumps(checks,indent=2)+'\n')
print(json.dumps(results,indent=2))
