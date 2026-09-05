from pathlib import Path
import json,hashlib,subprocess
P=Path(__file__).resolve().parent
def sha(q):return hashlib.sha256(q.read_bytes()).hexdigest()
inputs=json.loads((P/'inputs.json').read_text())
for path,row in inputs['identities'].items():assert sha(Path(path))==row['sha256'],path
prior=json.loads((P/'prior/manifest.json').read_text());base=Path(prior['package'])
for row in prior['artifacts']:assert sha(base/row['path'])==row['sha256'],row['path']
argv=['git','apply','--reverse','--check',str(P/'fixture-cleanup.patch')]
r=subprocess.run(argv,cwd=P/'candidate',capture_output=True,text=True);assert r.returncode==0,r.stderr
(P/'verification.json').write_text(json.dumps(dict(input_hashes_reverified=len(inputs['identities']),prior_manifest_sha256=sha(P/'prior/manifest.json'),prior_artifacts_reverified=len(prior['artifacts']),reverse_check=dict(argv=argv,cwd=str(P/'candidate'),exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr)),indent=2)+'\n')
art=[]
for q in sorted(P.rglob('*')):
 if not q.is_file() or q==P/'proposal-manifest.json':continue
 data=q.read_bytes();binary=data.startswith(b'\x7fELF') or data.startswith(b'!<arch>')
 if not binary:data.decode()
 art.append(dict(path=str(q.relative_to(P)),sha256=sha(q),bytes=len(data),storage='hash-only' if binary else 'text'))
(P/'proposal-manifest.json').write_text(json.dumps(dict(status='frozen read-only claim witness and unbuilt fixture cleanup proposal',package=str(P),artifacts=art),indent=2)+'\n')
print(json.dumps(dict(manifest_sha256=sha(P/'proposal-manifest.json'),proposal_sha256=sha(P/'PROPOSAL.md'),artifacts=len(art)),indent=2))
