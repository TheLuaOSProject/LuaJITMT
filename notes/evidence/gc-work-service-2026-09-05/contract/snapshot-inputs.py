from pathlib import Path
import json, hashlib, shutil, difflib
P = Path(__file__).resolve().parent
ROOT = Path('/tmp/lj-gc-workclass-fair-repair-20260905-q5riyfsd')
EVIDENCE = Path('/tmp/lj-gc-workclass-fairness-20260905-atmybi8c')
PRIOR = Path('/tmp/lj-gc-service-receipts-20260905-6f17l1ct')
def identity(p):
    b = p.read_bytes()
    return {'sha256': hashlib.sha256(b).hexdigest(), 'bytes': len(b)}
def put(src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    assert not dest.exists(), dest
    shutil.copyfile(src, dest)
    return identity(dest)
ours = json.loads((P/'setup.json').read_text())
theirs = json.loads((ROOT/'setup.json').read_text())
assert theirs['candidate_patch_sha256'] == 'eb3003c2e9f277487cde6308560fd4ba5aea2a33c3b73e5fa281cf3a778034a7'
assert theirs['head'] == ours['head'] == 'f9ec0a7217fc1a7eca61e17ab783bf32b9be1c61'
assert set(theirs['baseline_inputs']) == set(theirs['candidate_inputs']) == set(ours['source_inputs'])
changed = []
for rel, expected in ours['source_inputs'].items():
    assert identity(P/'base'/rel) == expected, rel
    assert identity(ROOT/'base'/rel)['sha256'] == theirs['baseline_inputs'][rel] == expected['sha256'], rel
    actual = identity(ROOT/'candidate'/rel)
    assert actual['sha256'] == theirs['candidate_inputs'][rel], rel
    if actual['sha256'] != expected['sha256']:
        changed.append(rel)
assert sorted(changed) == ['src/lj_gc2.c', 'src/lj_obj.h']
copies = {}
for rel in ['setup.json', 'candidate-v2.patch', 'candidate/src/lj_gc2.c', 'candidate/src/lj_obj.h', 'base/tests/t-gc2-traverse.c', 'diagnosis-v2/traverse-gdb.json']:
    copies['review-inputs/'+rel] = put(ROOT/rel, P/'review-inputs'/rel)
patch = ''.join(''.join(difflib.unified_diff((P/'base'/rel).read_text().splitlines(True), (P/'review-inputs/candidate'/rel).read_text().splitlines(True), fromfile='a/'+rel, tofile='b/'+rel)) for rel in changed)
assert patch.encode() == (P/'review-inputs/candidate-v2.patch').read_bytes()
verified = []
for src, expected_sha, dest in [(EVIDENCE,'89332888201e6b29a02a8cb2f851b2d81e3e46111af7645c1f4a6869c9f305a0','parent-baseline'), (PRIOR,'d8e2e16934ce8a3c8819a62e02c818ff7031a4753061582017a67ecb0a9b2980','prior-receipts')]:
    mf = src/'artifact-manifest.json'
    assert identity(mf)['sha256'] == expected_sha
    entries = json.loads(mf.read_text())['entries']
    for rel, ident in entries.items():
        assert identity(src/rel) == ident, (src, rel)
    verified.append({'package':str(src),'manifest_sha256':expected_sha,'entries_verified':len(entries)})
    copies[dest+'/artifact-manifest.json'] = put(mf, P/dest/'artifact-manifest.json')
    if src == EVIDENCE:
        selected = [rel for rel in entries if not rel.endswith('/fixture')]
    else:
        selected = ['HANDOFF.md','RECEIPT-MAP.md','REFACTOR.md']
    for rel in selected:
        copies[dest+'/'+rel] = put(src/rel,P/dest/rel)
report = {'scope':'read-only source review; no runtime execution, patch application or build', 'head':ours['head'], 'base_inputs_verified':len(ours['source_inputs']), 'root_base_inputs_verified':len(theirs['baseline_inputs']), 'root_candidate_inputs_verified':len(theirs['candidate_inputs']), 'changed_inputs':changed, 'patch_reconstruction_exact':True, 'candidate_patch_sha256':identity(P/'review-inputs/candidate-v2.patch')['sha256'], 'copied_artifacts':copies,'prior_manifests_verified':verified}
(P/'source-and-evidence-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ['copied_artifacts']},indent=2))
print('copied_artifacts',len(copies))
