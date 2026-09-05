from pathlib import Path
import hashlib,json
p=Path(__file__).resolve().parent
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
assert not (p/'artifact-manifest.json').exists()
identity=json.loads((p/'source-identity.json').read_text())
runtime={}
for variant in ['candidate','optimized','asan']:
    for name, values in identity['sources'].items():
        assert sha(p/variant/name)==values['candidate_sha256'], (variant,name)
    runtime[variant]={name:sha(p/variant/name) for name in ['src/luajit','src/libluajit.a','src/jit/vmdef.lua']}
rows=[]
def add(path, category, final):
    data=json.loads((p/path).read_text())
    if isinstance(data,dict): data=[data]
    for r in data:
        rows.append({'record':path,'category':category,'final':final,
                     'command':r['command'],'exit':r['exit'],'seconds':r['seconds']})
add('idle-initial-results.json','idle',True)
for folder in sorted((p/'validation').iterdir()):
    f=folder/'results.json'
    if not f.exists(): continue
    name=folder.name
    if name.startswith('authority-candidate-v') and name!='authority-candidate-v6':
        cat='fixture-development'; final=False
    elif name=='progress-baseline-complete': cat='baseline-negative-control'; final=False
    else: cat=name.split('-')[0]; final=True
    add(str(f.relative_to(p)),cat,final)
    if name=='progress-candidate-v1':
        for r in rows:
            if r['record']==str(f.relative_to(p)) and r['command'][-2:]==['capi','dense']:
                r['category']='unresolved-C-API-progress'; r['final']=False
for variant in runtime: add('stock-'+variant+'-results.json','stock',True)
finalrows=[r for r in rows if r['final']]
assert len(finalrows)==123 and all(r['exit']==0 for r in finalrows)
summary={'base':'79345529','source_patch_sha256':sha(p/'candidate-v1.patch'),
         'source_count_per_build':224,'runtime':runtime,'final_runtime_passes':len(finalrows),
         'all_recorded_runtime_processes':len(rows),'rows':rows,
         'preserved_failed_compiles':[str(f.relative_to(p)) for f in sorted((p/'validation').glob('*/compile.json')) if json.loads(f.read_text())['exit']!=0],
         'observer':'capi-observer.json; read-only interruption, not runtime pass',
         'known_runtime_limit':'Public C lua_next blocks before the new iterator, in unchanged API receiver capture.'}
(p/'validation-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
files={}
for f in sorted(p.rglob('*')):
    if not f.is_file(): continue
    rel=f.relative_to(p)
    if rel.parts[0] in ['candidate','optimized','asan']:
        if '/'.join(rel.parts[1:]) not in ['src/luajit','src/libluajit.a','src/jit/vmdef.lua']: continue
    if '__pycache__' in rel.parts or rel.name in ['artifact-manifest.json','final-verification.json']: continue
    files[str(rel)]=sha(f)
(p/'artifact-manifest.json').write_text(json.dumps({'base':'79345529','files':files},indent=2)+'\n')
print('frozen',len(files),'artifacts;',len(finalrows),'final passes;',len(rows),'all runtime records')
