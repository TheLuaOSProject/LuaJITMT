from pathlib import Path
import hashlib,json,subprocess
P=Path('/tmp/lj-reclaim-owner-defer-20260905-gwiiudxk')
def sha(q):return hashlib.sha256(q.read_bytes()).hexdigest()
record=json.loads((P/'source-identity.json').read_text());S=Path(record['source_tree'])
changes=[]
for rel,want in record['inputs'].items():
 assert sha(S/rel)==want,(rel,'baseline modified')
 if sha(P/'candidate'/rel)!=want:changes.append(rel)
assert changes==['src/lj_gc.c','src/lj_gc.h','src/lj_gc2.c'],changes
r=subprocess.run(['git','apply','--check','--reverse',str(P/'candidate.patch')],cwd=P/'candidate',text=True,capture_output=True)
assert r.returncode==0,r.stderr
validation={'input_count':len(record['inputs']),'changed':changes,'reverse_patch_check':{'argv':r.args,'cwd':str(P/'candidate'),'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr},'compiler':subprocess.check_output(['cc','--version'],text=True),'baseline_original_executable':{}}
original=Path('/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a/control-matched-t-func-construction-anchor')
validation['baseline_original_executable']={'path':str(original),'sha256':sha(original),'new_baseline_sha256':sha(P/'baseline-t-func-construction-anchor'),'byte_identical':sha(original)==sha(P/'baseline-t-func-construction-anchor')}
(P/'focused-source-validation.json').write_text(json.dumps(validation,indent=2)+'\n')
art=[]
for f in sorted(P.rglob('*')):
 if not f.is_file():continue
 rel=f.relative_to(P)
 if str(rel)=='focused-manifest.json':continue
 if rel.parts[0]=='candidate' and str(rel) not in ('candidate/src/lj_gc.c','candidate/src/lj_gc.h','candidate/src/lj_gc2.c','candidate/tests/t-func-construction-anchor.c','candidate/src/libluajit.a','candidate/src/luajit'):continue
 data=f.read_bytes();binary=data.startswith((b'\x7fELF',b'!<arch>\n'))
 if not binary:data.decode()
 art.append({'path':str(rel),'bytes':len(data),'sha256':sha(f),'storage':'hash-only' if binary else 'text'})
manifest={'status':'frozen first focused checkpoint; broader acceptance evidence pending','package':str(P),'artifacts':art}
(P/'focused-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'patch_sha256':sha(P/'candidate.patch'),'manifest_sha256':sha(P/'focused-manifest.json'),'artifacts':len(art),'baseline_byte_identical':validation['baseline_original_executable']['byte_identical']},indent=2))
