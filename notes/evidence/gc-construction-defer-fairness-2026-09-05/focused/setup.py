from pathlib import Path
import tempfile,json,hashlib,shutil
P=Path(tempfile.mkdtemp(prefix='lj-reclaim-fair-pass-20260905-'))
Path('/tmp/lj-reclaim-fair-pass-current').write_text(str(P)+'\n')
S=Path('/tmp/lj-reclaim-owner-defer-20260905-gwiiudxk')
identity=json.loads((S/'source-identity.json').read_text());inputs={}
for rel in identity['inputs']:
 q=S/'candidate'/rel;dest=P/'candidate'/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(q,dest)
 inputs[rel]=hashlib.sha256(q.read_bytes()).hexdigest()
(P/'source-identity.json').write_text(json.dumps({'pristine_base':identity['base'],'starting_source':str(S/'candidate'),'starting_patch_sha256':hashlib.sha256((S/'candidate.patch').read_bytes()).hexdigest(),'inputs':inputs},indent=2)+'\n')
(P/'prior').mkdir()
for name in ['candidate.patch','focused-manifest.json','acceptance-manifest.json','ACCEPTANCE-HANDOFF.md']:
 shutil.copyfile(S/name,P/'prior'/name)
(P/'setup.py').write_text(Path('/tmp/lj-reclaim-fair-setup.py').read_text())
print(P)
