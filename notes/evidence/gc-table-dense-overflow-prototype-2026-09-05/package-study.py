#!/usr/bin/env python3
"""Preserve existing study evidence; no runtime builds or workload execution."""
from pathlib import Path
from collections import Counter
import hashlib, json, subprocess, shutil, io, tarfile

repo=Path('/workspaces/lj-lockless')
original=Path('/tmp/lj-dense-overflow-20260905-7tl6kcfk')
work=Path(__file__).resolve().parent
base=work/'base'
name='gc-table-dense-overflow-prototype-2026-09-05'
evidence=repo/'notes/evidence'/name
bench=repo/'bench'/name
base_ref='d680421c4cb50b85437d88255bc89358c5e3a6b1'
commands=[]

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def write_json(p,v): p.write_text(json.dumps(v,indent=2)+'\n')
def run(cmd,cwd):
    q=subprocess.run(cmd,cwd=cwd,text=True,capture_output=True)
    commands.append({'command':cmd,'cwd':str(cwd),'exit':q.returncode,
                     'stdout':q.stdout,'stderr':q.stderr})
    assert q.returncode==0, commands[-1]
    return q

evidence.mkdir(parents=True,exist_ok=True);bench.mkdir(parents=True,exist_ok=True)
archive_command=['git','archive',base_ref,'src','dynasm','plan/aux/bench','tests']
archive=subprocess.run(archive_command,cwd=repo,capture_output=True)
assert archive.returncode==0,archive.stderr
commands.append({'command':archive_command,'cwd':str(repo),'exit':archive.returncode,
    'archive_bytes':len(archive.stdout),'archive_sha256':hashlib.sha256(archive.stdout).hexdigest(),
    'stderr':archive.stderr.decode()})
base.mkdir(exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tf:
    tf.extractall(base,filter='data')
final=json.loads((original/'final-validation.json').read_text())
assert final['base']==base_ref
verified_original=[]
for variant, entries in final['source_variants'].items():
    for f,h in entries.items():
        assert sha(original/variant/f)==h,(variant,f)
        verified_original.append(variant+'/'+f)
for group in ['fixtures','binaries','evidence']:
    for f,h in final[group].items():
        assert sha(original/f)==h,(group,f)
        verified_original.append(f)

index=[]
for p in sorted(original.iterdir()):
    if not p.is_file() or p.name=='d680.tar': continue
    content=p.read_bytes()
    if b'\0' in content: continue
    content.decode('utf-8')
    destroot=bench if p.name.startswith(('cost','compile-cost','summarize-cost','promoted-memory')) else evidence
    dest=destroot/p.name
    dest.write_bytes(content)
    index.append({'original_path':str(p),'repository_path':str(dest.relative_to(repo)),
                  'bytes':len(content),'sha256':sha(p)})
assert len(index)==82,len(index)
write_json(evidence/'original-artifact-index.json',{'original_directory':str(original),'files':index})

paths=run(['git','ls-tree','-r','--name-only',base_ref,'--','src','dynasm'],repo).stdout.splitlines()
base_sources={f:sha(base/f) for f in paths}
changed=['src/lj_arena.c','src/lj_arena.h','src/lj_gc2.c','src/lj_gc2.h']
tested_manifest=json.loads((original/'source-manifest.json').read_text())
tested={x['path']:x['sha256'] for x in tested_manifest['files']}
candidate_manifest=json.loads((original/'candidate-source-manifest.json').read_text())
candidate={x['path']:x['sha256'] for x in candidate_manifest['files']}
assert sha(original/'dense-W.patch')==tested_manifest['patch_sha256']
assert sha(original/'dense-W-candidate.patch')==candidate_manifest['patch_sha256']

reconstructions={}
for variant,patch,expected in [
    ('tested','dense-W.patch',tested),('candidate','dense-W-candidate.patch',candidate)]:
    dst=work/('reconstructed-'+variant)
    shutil.copytree(base,dst,dirs_exist_ok=True)
    run(['git','apply','--check',str(original/patch)],dst)
    run(['git','apply',str(original/patch)],dst)
    actual={f:sha(dst/f) for f in changed}
    assert actual==expected,(variant,actual)
    reconstructions[variant]={'patch':patch,'source_sha256':actual}
comment_tree=work/'reconstructed-comment'
shutil.copytree(work/'reconstructed-tested',comment_tree,dirs_exist_ok=True)
run(['git','apply','--check',str(original/'accessor-comment.patch')],comment_tree)
run(['git','apply',str(original/'accessor-comment.patch')],comment_tree)
assert all(sha(comment_tree/f)==candidate[f] for f in changed)
reconstructions['comment_only_matches_candidate']=True

variant_inventory={}
for variant in ['base-normal','strict','normal','asan']:
    expected=dict(base_sources)
    if variant!='base-normal':expected.update(tested)
    for f,h in expected.items():assert sha(original/variant/f)==h,(variant,f)
    variant_inventory[variant]={'tracked_files_checked':len(paths),
        'base':base_ref,'overrides':{} if variant=='base-normal' else tested}
for mode in ['inline','wide','mode']:
    variant='negative-'+mode;dst=work/('reconstructed-'+variant)
    shutil.copytree(work/'reconstructed-tested',dst,dirs_exist_ok=True)
    run(['git','apply','--check',str(original/(variant+'.patch'))],dst)
    run(['git','apply',str(original/(variant+'.patch'))],dst)
    expected=dict(base_sources);expected.update(tested)
    expected['src/lj_gc2.c']=sha(dst/'src/lj_gc2.c')
    for f,h in expected.items():assert sha(original/variant/f)==h,(variant,f)
    variant_inventory[variant]={'tracked_files_checked':len(paths),'base':base_ref,
        'overrides':{f:expected[f] for f in changed},'negative_patch_sha256':sha(original/(variant+'.patch'))}
    reconstructions[variant]={'patch':variant+'.patch','source_sha256':{f:expected[f] for f in changed}}

# Supplement existing manifests with the complete tracked source namespace and
# packaging-time identities for generated files and negative/remaining fixtures.
generated={}
for variant in variant_inventory:
    src=original/variant/'src'
    for p in sorted(src.rglob('*')):
        if not p.is_file(): continue
        rel='src/'+str(p.relative_to(src))
        if (p.suffix in ('.o','.a','.so') or p.name in ('luajit','buildvm','minilua') or
            (p.suffix=='.h' and rel not in base_sources)):
            generated[variant+'/'+rel]={'bytes':p.stat().st_size,'sha256':sha(p)}
for p in sorted(original.iterdir()):
    if p.is_file() and p.name!='d680.tar' and b'\0' in p.read_bytes()[:128]:
        generated[p.name]={'bytes':p.stat().st_size,'sha256':sha(p)}

dependencies={}
for f in ['tests/lib/lua_fixture_helpers.h','tests/lib/thread_fixture_helpers.h',
          'tests/t-gc2-recovery.c','tests/t-gc2-table-store-guard.c',
          'tests/t-gc2-sweep-table-coalescing.c','tests/t-gc2-traverse.c',
          'tests/t-x64-tnew-empty-inline.c','tests/t-jit-fnew-bump.c','plan/aux/bench/bench.lua']:
    if not (base/f).exists():continue
    h=sha(base/f)
    for variant in ['base-normal','strict','normal','asan']:
        assert sha(original/variant/f)==h,(variant,f)
    dependencies[f]=h
plan=original/'normal/plan/aux/bench/bench.lua'
assert sha(plan)==sha(original/'base-normal/plan/aux/bench/bench.lua')==sha(base/'plan/aux/bench/bench.lua')
(bench/'plan-bench.lua').write_bytes(plan.read_bytes())
write_json(evidence/'supplemental-snapshot.json',{
    'scope':'Packaging-time read-only inventory; not a new runtime or build execution.',
    'base':base_ref,'base_tracked_source_files':base_sources,
    'variants':variant_inventory,'generated_and_binary_files':generated,
    'unchanged_dependencies':dependencies,'original_base_tar_sha256':sha(original/'d680.tar')})

rows=json.loads((original/'cost-results.json').read_text())
assert len(rows)==153
assert all(r['status']=='complete' and r['exit']==0 for r in rows)
assert Counter(r['kind'] for r in rows)==Counter(barrier=21,plan=84,memory=48)
for r in rows:
    assert r['command'][:3]==['taskset','-c','31']
    if r['kind']=='barrier':
        d=next(d for d in r['data'] if d['type']=='barrier')
        assert d['n']==1000000 and d['dirty']-d['dirty_start']==1000000
        assert d['pending']==0 and d['recovery']==0
    for d in r.get('data',[]):
        if d['type']=='memory':
            assert d['metadata_denied']==0
            if d['stage'].endswith('_collected'):
                assert d['phase']==0 and d['recovery']==0
            if r['kind']=='memory':assert d['huge_w_requested']==0 and d['huge_w_usable']==0

# Rerun the analysis script only, in a separate directory, on frozen raw data.
analysis=work/'summary-regeneration';analysis.mkdir(exist_ok=True)
for f in ['cost-results.json','summarize-cost.py']:(analysis/f).write_bytes((original/f).read_bytes())
q=run(['python3',str(analysis/'summarize-cost.py')],analysis)
assert (analysis/'cost-summary.json').read_bytes()==(original/'cost-summary.json').read_bytes()
(bench/'summary-regeneration.stdout').write_text(q.stdout)
(bench/'summary-regeneration.stderr').write_text(q.stderr)

classifications={}
for f in ['strict-results.json','asan-results.json']:
    records=json.loads((original/f).read_text());assert all(x['exit']==0 for x in records)
    classifications[f]={'records':len(records),'exit_zero':len(records),
        'diagnostic_only':['fnew-existing'],
        'qualification':'Settled inherited full FNEW setup is not valid protocol evidence; use fnew-consistent-results.json.'}
for f in ['negative-results.json','fnew-consistent-results.json']:
    records=json.loads((original/f).read_text())
    nonzero=[x for x in records if x['exit']]
    assert len(nonzero)==(3 if f=='negative-results.json' else 2)
    assert all(x['exit']==-6 and x['status']=='complete' for x in nonzero)
    classifications[f]={'records':len(records),'expected_assertions':[x['name'] for x in nonzero]}
for f in ['stock-results.json','base-stock-results.json']:
    records=json.loads((original/f).read_text())
    assert len(records)==2 and all(x['exit']==0 for x in records)
    assert records[0]['stdout'].strip()=='387 passed'
    assert records[1]['stdout'].strip()=='509 passed'
    classifications[f]={'jit_off':387,'jit_on':509,'all_exit_zero':True}

write_json(bench/'source-snapshot.json',{
    'base':base_ref,'frozen_source_variants':{v:final['source_variants'][v] for v in ['base-normal','normal']},
    'binaries':{k:h for k,h in final['binaries'].items() if k.startswith(('base-normal','normal'))},
    'builds':{v:final['builds'][v] for v in ['base-normal','normal']},
    'tools':final['tools'],'source_files':{f:sha(original/f) for f in ['cost.c','cost-study.py','summarize-cost.py','compile-cost.py']},
    'plan_harness':{'base_path':'plan/aux/bench/bench.lua','packaged_path':'plan-bench.lua','sha256':sha(plan)},
    'tested_patch_sha256':sha(original/'dense-W.patch'),
    'candidate_difference':'Only the small-only accessor comment; candidate patch was not measured.',
    'original_final_validation_sha256':sha(original/'final-validation.json'),
    'runtime_source_boundary':'Exact d680 plus dense-W.patch, excluding later arena/scalar/direct-hit and Huge-tail changes.'})
write_json(evidence/'packaging-validation.json',{
    'scope':'Read-only verification of existing frozen study plus durable text packaging; no runtime builds/tests or timings rerun.',
    'original_directory':str(original),'verification_directory':str(work),
    'original_manifest_hashes_verified':len(verified_original),
    'original_manifest_items_verified':verified_original,
    'copied_original_text_artifacts':len(index),'copied_original_text_bytes':sum(x['bytes'] for x in index),
    'all_original_text_unchanged':all(sha(Path(x['original_path']))==x['sha256'] for x in index),
    'source_patch_reconstructions':reconstructions,
    'complete_tracked_source_files_per_variant':len(paths),
    'source_variants_checked':list(variant_inventory),'result_classifications':classifications,
    'cost_samples':len(rows),'cost_kinds':dict(Counter(r['kind'] for r in rows)),
    'cost_all_complete_exit_zero':True,'cost_barrier_exact_increments_and_drained':True,
    'cost_all_recorded_metadata_admissions_succeeded':True,'cost_settled_memory_idle_recovery_zero':True,
    'cost_no_traversable_huge_w_allocations':True,
    'regenerated_cost_summary_sha256':sha(analysis/'cost-summary.json'),
    'original_cost_summary_sha256':sha(original/'cost-summary.json'),
    'commands':commands})
# Save this packaging driver as a record; its absolute destinations are not a
# request to rerun it over frozen evidence.
(evidence/'package-study.py').write_bytes(Path(__file__).read_bytes())
print('Copied',len(index),'original text artifacts;',len(verified_original),'original manifest entries verified')
print('Verified',len(paths),'tracked source files in each of',len(variant_inventory),'variants')
print('Reconstructed tested, comment-only candidate and three negative variants')
print('Recomputed exact cost summary from',len(rows),'completed processes; no runtime executed')
print('Evidence',evidence)
print('Bench',bench)
