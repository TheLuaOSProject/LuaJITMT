from pathlib import Path
import json, hashlib
P = Path(__file__).resolve().parent
def ident(p):
    b=p.read_bytes()
    return {'bytes':len(b), 'sha256':hashlib.sha256(b).hexdigest()}
setup=json.loads((P/'setup.json').read_text())
root=json.loads((P/'review-inputs/setup.json').read_text())
for rel,expected in setup['source_inputs'].items():
    assert ident(P/'base'/rel)==expected, rel
for rel,expected in root['candidate_inputs'].items():
    candidate=P/'review-inputs/candidate'/rel
    if not candidate.exists(): candidate=P/'base'/rel
    assert ident(candidate)['sha256']==expected, rel
v=json.loads((P/'source-and-evidence-verification.json').read_text())
for rel,expected in v['copied_artifacts'].items():
    assert ident(P/rel)==expected, rel
for info in v['prior_manifests_verified']:
    origin=Path(info['package'])
    assert ident(origin/'artifact-manifest.json')['sha256']==info['manifest_sha256']
    for rel,expected in json.loads((origin/'artifact-manifest.json').read_text())['entries'].items():
        assert ident(origin/rel)==expected, (origin,rel)
entries={str(f.relative_to(P)):ident(f) for f in sorted(P.rglob('*')) if f.is_file() and f.name not in ['artifact-manifest.json','manifest-verification.json']}
manifest={'scope':'source-only review of exact ROOT fairness v2; no reviewer runtime validation; runtime unlanded/provisional','base_head':setup['head'],'candidate_patch_sha256':root['candidate_patch_sha256'],'entries':entries}
(P/'artifact-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
for rel,expected in entries.items():assert ident(P/rel)==expected,rel
out={'entries_verified':len(entries),'base_inputs_verified':len(setup['source_inputs']),'candidate_overlay_inputs_verified':len(root['candidate_inputs']),'copied_inputs_verified':len(v['copied_artifacts']),'prior_manifests_reverified':v['prior_manifests_verified'],'manifest_sha256':ident(P/'artifact-manifest.json')['sha256'],'runtime_tests_executed_by_reviewer':0,'builds_executed_by_reviewer':0,'debugger_sessions_executed_by_reviewer':0,'patches_applied_by_reviewer':0}
(P/'manifest-verification.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
for rel in ['HANDOFF.md','CONTRACT.md','AUTHORITY.md','REVIEW-V2.md','VALIDATION-CONTRACT.md']:
    print(rel,ident(P/rel)['sha256'])
