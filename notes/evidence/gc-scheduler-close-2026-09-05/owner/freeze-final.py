from pathlib import Path
import json,hashlib,subprocess
P=Path(__file__).resolve().parent
def sha(q):return hashlib.sha256(q.read_bytes()).hexdigest()
prior=json.loads((P/'proposal-manifest.json').read_text())
for row in prior['artifacts']:assert sha(P/row['path'])==row['sha256'],row['path']
verified={}
for d in ['validation','validation-793-fixture-headers']:
 ids=json.loads((P/d/'identities.json').read_text())
 for path,row in ids.items():assert sha(Path(path))==row['sha256'],path
 verified[d]=len(ids)
argv=['git','apply','--reverse','--check',str(P/'fixture-cleanup.patch')]
r=subprocess.run(argv,cwd=P/'candidate',capture_output=True,text=True);assert r.returncode==0,r.stderr
summary=json.loads((P/'final-runtime-summary.json').read_text());assert len(summary)==8
assert (P/'positive-witness.stderr').stat().st_size==0
(P/'final-verification.json').write_text(json.dumps(dict(proposal_manifest_sha256=sha(P/'proposal-manifest.json'),proposal_artifacts_reverified=len(prior['artifacts']),input_identities_reverified=verified,runtime_passes=len(summary),asan_lsan_passes=sum(r['ASAN_OPTIONS'] is not None for r in summary),reverse_check=dict(argv=argv,cwd=str(P/'candidate'),exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr)),indent=2)+'\n')
art=[]
for q in sorted(P.rglob('*')):
 if not q.is_file() or q==P/'final-manifest.json':continue
 data=q.read_bytes();binary=data.startswith(b'\x7fELF') or data.startswith(b'!<arch>')
 if not binary:data.decode()
 art.append(dict(path=str(q.relative_to(P)),sha256=sha(q),bytes=len(data),storage='hash-only' if binary else 'text'))
(P/'final-manifest.json').write_text(json.dumps(dict(status='frozen validated fixture cleanup; eight full passes including four ASan/LSan, actual real-close witness',package=str(P),artifacts=art),indent=2)+'\n')
print(json.dumps(dict(manifest_sha256=sha(P/'final-manifest.json'),handoff_sha256=sha(P/'HANDOFF.md'),artifacts=len(art),text=sum(r['storage']=='text' for r in art),hash_only=sum(r['storage']=='hash-only' for r in art)),indent=2))
