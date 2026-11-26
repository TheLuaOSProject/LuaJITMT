from pathlib import Path
import hashlib,json,subprocess
p=Path(__file__).resolve().parent
root=Path('/workspaces/lj-lockless')
def sha(f):return hashlib.sha256(Path(f).read_bytes()).hexdigest()
report={'base':json.loads((p/'setup.json').read_text())['base'],'builds':{},'canonical':{}}
for mode in ['combined-v1','assert-v1','asan-v1','tsan-v1','nojit-v1','noffi-v1','nobuf-v1']:
 tree=p/mode; meta=json.loads((tree/'build.json').read_text());assert meta['exit']==0
 source={rel:old for rel,old in meta['source_inputs'].items() if rel=='Makefile' or rel.startswith(('src/','dynasm/'))}
 mismatches=[rel for rel,old in source.items() if sha(root/rel)!=old];assert not mismatches,(mode,mismatches)
 for name,old in meta['outputs'].items():assert sha(tree/'src'/name)==old,(mode,name)
 runs=[]
 for f in sorted(tree.glob('check-*/results.json')):
  for r in json.loads(f.read_text()):
   assert r['exit']==0,(mode,f,r)
   runs.append({'phase':f.parent.name,'name':r['name'],'exit':r['exit'],'runtime':r.get('test',True)})
 assert runs,mode
 report['builds'][mode]={'runtime_input_count':len(source),'source_mismatches':mismatches,'binary_sha256':meta['outputs'],'checks':runs}
meta=json.loads((p/'canonical/result.json').read_text());assert meta['exit']==0 and not meta['source_mismatches']
for name,old in meta['outputs'].items():assert sha(root/'src'/name)==old,name
report['canonical']=meta
report['tracked_changes']=subprocess.check_output(['git','diff','--stat'],cwd=root,text=True)
report['runtime_checks']=sum(sum(r['runtime'] for r in b['checks']) for b in report['builds'].values())
(p/'final-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print('Verified',len(report['builds']),'builds;',report['runtime_checks'],'runtime invocations; canonical/default hashes match')
