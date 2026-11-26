from pathlib import Path
import hashlib,json,shutil,subprocess,tarfile
p=Path(__file__).resolve().parent;root=Path('/workspaces/lj-lockless');dest=root/'notes/evidence/buffer-concurrent-storage-2026-11-26';dest.mkdir(parents=True,exist_ok=False)
sha=lambda b:hashlib.sha256(b).hexdigest()
verification={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'builds':{},'performance':{}}
for mode in ['gcc','assert','asan','tsan','noffi','nojit','nobuf']:
 d=p/(mode+'-v4');m=json.loads((d/'build.json').read_text());assert m['exit']==0
 runtime={f:h for f,h in m['source_inputs'].items() if f=='Makefile' or f.startswith(('src/','dynasm/'))}
 for rel,h in runtime.items():assert sha((root/rel).read_bytes())==h,rel
 for name,h in m['outputs'].items():assert sha((d/'src'/name).read_bytes())==h,name
 rows=[]
 for f in d.glob('check-*/results.json'):
  for r in json.loads(f.read_text()):assert r['exit']==0,(mode,f,r);rows.append(r)
 verification['builds'][mode]={'runtime_input_count':len(runtime),'rows':len(rows),'runtime_processes':sum(r.get('test',True) for r in rows)}
canonical=json.loads((p/'canonical-v4/result.json').read_text());assert canonical['exit']==0 and not canonical['source_mismatches']
for name,h in canonical['outputs'].items():assert sha((root/'src'/name).read_bytes())==h,name
verification['canonical']=canonical
perf=Path('/tmp/lj-buffer-perf-v4-20261126-32pd9m6x');meta=json.loads((perf/'metadata.json').read_text())
for name,h in meta['artifact_sha256'].items():assert sha((perf/name).read_bytes())==h,name
for name,h in meta['binary_sha256'].items():assert sha((perf/('baseline-luajit' if name=='baseline' else 'candidate-luajit-v4')).read_bytes())==h,name
for name,baseline in [('candidate-input.tar.gz',False),('baseline-input.tar',True)]:
 count=0
 with tarfile.open(perf/name) as t:
  for m in t.getmembers():
   if not m.isfile():continue
   b=t.extractfile(m).read()
   expected=subprocess.check_output(['git','show',meta['baseline_commit']+':'+m.name],cwd=root) if baseline else (root/m.name).read_bytes()
   assert b==expected,m.name;count+=1
 verification['performance'][name]=count
(p/'final-verification.json').write_text(json.dumps(verification,indent=2)+'\n')
(p/'final-runtime.patch').write_bytes(subprocess.check_output(['git','diff','--','src'],cwd=root))
entries=[]
def add(source,rel):
 b=source.read_bytes();entry=dict(path=str(rel),source=str(source),sha256=sha(b),bytes=len(b))
 try:
  b.decode('utf-8');istext=b'\0' not in b and not b.startswith((b'\x7fELF',b'!<arch>'))
 except UnicodeDecodeError:istext=False
 entry['stored']='text' if istext else 'hash-only';entries.append(entry)
 if istext:
  out=dest/rel;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(b)
for f in p.iterdir():
 if f.is_file():add(f,Path('review')/f.name)
for dirname in ['canonical-v3','canonical-v4','dictionary-control']:
 for f in (p/dirname).rglob('*'):
  if f.is_file():add(f,Path('review')/f.relative_to(p))
for d in p.iterdir():
 if not d.is_dir() or not (d/'build.json').exists():continue
 add(d/'build.json',Path('validation')/d.name/'build.json')
 if d.name.endswith('-v4'):add(d/'build.log',Path('validation')/d.name/'build.log')
 for c in d.glob('check-*'):
  for f in c.rglob('*'):
   if f.is_file():add(f,Path('validation')/d.name/f.relative_to(d))
 if d.name.endswith('-v4'):
  for name in ['luajit','libluajit.a']:add(d/'src'/name,Path('validation')/d.name/'binary-identities'/name)
for version,d in [('v2',Path('/tmp/lj-buffer-perf-20261126-dt90tksg')),('v3',Path('/tmp/lj-buffer-perf-v3-20261126-v8v1ezme')),('v4',perf)]:
 for f in d.iterdir():
  if f.is_file():add(f,Path('performance')/version/f.name)
for f in Path('/tmp').glob('buffer-public-*'):
 if f.is_file() and f.suffix=='.log':add(f,Path('original-controls')/f.name)
manifest={'base_commit':verification['head'],'entries':entries}
(dest/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
for e in entries:
 if e['stored']=='text':assert sha((dest/e['path']).read_bytes())==e['sha256']
print(json.dumps(dict(archive=str(dest),text=sum(e['stored']=='text' for e in entries),hash_only=sum(e['stored']=='hash-only' for e in entries),text_bytes=sum(e['bytes'] for e in entries if e['stored']=='text'),manifest_sha256=sha((dest/'manifest.json').read_bytes())),indent=2))
