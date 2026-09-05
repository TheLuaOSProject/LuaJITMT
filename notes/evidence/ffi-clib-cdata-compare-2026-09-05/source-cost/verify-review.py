from pathlib import Path
import hashlib, json, subprocess
p=Path(__file__).resolve().parent
sha=lambda data: hashlib.sha256(data).hexdigest()
r=json.loads((p/'reviewed-inputs.json').read_text())
src=Path(json.loads((p/'scope.json').read_text())['source_root'])
repo=Path('/workspaces/lj-lockless')
for name,row in r['runtime_inputs'].items():
    assert sha((src/name).read_bytes())==row['frozen_sha256'], name
    data=subprocess.check_output(['git','show',r['commit']+':'+name],cwd=repo)
    assert sha(data)==row['committed_sha256']==row['frozen_sha256'],name
for name,row in r['extra_inputs'].items():
    path=Path(name)
    assert path.stat().st_size==row['size'] and sha(path.read_bytes())==row['sha256'],name
old=json.loads((p/'prior-package-integrity.json').read_text())
counts={}
for name,row in old.items():
    path=Path(name)
    assert sha(path.read_bytes())==row['sha256'],name
    manifest=json.loads(path.read_text())
    for entry in manifest['artifacts']:
        child=path.parent/entry['relative_path']
        assert child.stat().st_size==entry['bytes'],str(child)
        assert sha(child.read_bytes())==entry['sha256'],str(child)
    counts[name]=len(manifest['artifacts'])
manifest_path=p/'artifact-manifest.json'
if manifest_path.exists():
    for entry in json.loads(manifest_path.read_text())['artifacts']:
        child=p/entry['relative_path']
        assert child.stat().st_size==entry['bytes'],str(child)
        assert sha(child.read_bytes())==entry['sha256'],str(child)
print(json.dumps({'runtime_inputs':len(r['runtime_inputs']),'extra_inputs':len(r['extra_inputs']),'prior_artifact_counts':counts,'all_pass':True},indent=2))
