import difflib,hashlib,json,pathlib,subprocess,tempfile
r=pathlib.Path(__file__).resolve().parent
repo=pathlib.Path('/workspaces/lj-lockless')
base='8d342cd6456d2f93ba07a779cdde30d4806eb90f'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def blob(path): return subprocess.check_output(['git','show',base+':'+path],cwd=repo)
def text_diff(path,old,new,isnew=False):
    head=f'diff --git a/{path} b/{path}\n'
    if isnew: head+='new file mode 100644\n'
    return head+''.join(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='/dev/null' if isnew else 'a/'+path,tofile='b/'+path))
tests=['tests/t-jit-cdata-pure.lua','tests/t-jit-cdata-pure-side.lua','tests/t-jit-cdata-pure-exclusions.lua','tests/t-jit-cdata-pure-profile.lua','tests/t-jit-cdata-pure-phase.c','tests/t-jit-cdata-pure-error.c','tests/suites/m6_jit.lua']
patch=''
for p in tests:
    isnew=p!='tests/suites/m6_jit.lua'
    patch+=text_diff(p,'' if isnew else blob(p).decode(),(r/'tree'/p).read_text(),isnew)
(r/'tests-only.patch').write_text(patch)
paths=subprocess.check_output(['git','ls-tree','-r','--name-only',base,'src','dynasm'],cwd=repo,text=True).splitlines()
basehash={p:hashlib.sha256(blob(p)).hexdigest() for p in paths}
prod=['src/lj_jit.h','src/lj_opt_loop.c','src/lj_opt_mem.c']
variants={}
for v in ['tree','strict']:
    hashes={p:sha(r/v/p) for p in paths}
    changes=[p for p in paths if hashes[p]!=basehash[p]]
    assert sorted(changes)==sorted(prod),(v,changes)
    variants[v]={'runtime_input_sha256':hashes,'modified_from_base':changes,'runtime_sha256':sha(r/v/'src/luajit'),'archive_sha256':sha(r/v/'src/libluajit.a')}
assert variants['tree']['runtime_input_sha256']==variants['strict']['runtime_input_sha256']
reconstruction=pathlib.Path(tempfile.mkdtemp(prefix='source-reconstruction-',dir=r))
for p in prod:
    out=reconstruction/p;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(blob(p))
cmd=['git','apply','--verbose','--whitespace=error-all',str(r/'implementation.patch')]
res=subprocess.run(cmd,cwd=reconstruction,capture_output=True,text=True)
assert res.returncode==0,res.stderr
assert all(sha(reconstruction/p)==sha(r/'tree'/p) for p in prod)
(r/'source-reconstruction.json').write_text(json.dumps({'command':cmd,'cwd':str(reconstruction),'exit':res.returncode,'stdout':res.stdout,'stderr':res.stderr,'all_three_modified_sources_match':True},indent=2)+'\n')
check=['git','apply','--check','--whitespace=error-all',str(r/'tests-only.patch')]
res=subprocess.run(check,cwd=repo,capture_output=True,text=True)
assert res.returncode==0,res.stderr
(r/'tests-apply-check.json').write_text(json.dumps({'command':check,'cwd':str(repo),'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),'exit':res.returncode,'stdout':res.stdout,'stderr':res.stderr},indent=2)+'\n')
manifest={'base_commit':base,'implementation_patch_sha256':sha(r/'implementation.patch'),'tests_patch_sha256':sha(r/'tests-only.patch'),'runtime_input_count':len(paths),'baseline_runtime_input_sha256':basehash,'variants':variants,'final_test_source_root':str(r/'tree'),'final_tests_sha256':{p:sha(r/'tree'/p) for p in tests},'c_executables_sha256':{str(p.relative_to(r)):sha(p) for p in [r/'strict-phase',r/'strict-error',r/'tmp/lj_t-jit-cdata-pure-phase',r/'tmp/lj_t-jit-cdata-pure-error']},'strict_note':'The strict driver uses final test sources from tree/tests; strict tree copies predate C-only formatting. Runtime input identity is exact in both trees.','normal_build_note':'Initial static binary hashes are in build-results.json. Canonical build_default rebuilds normal tree with default mixed flags; these are final canonical binary hashes.'}
(r/'final-source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
rows=json.loads((r/'strict-results.json').read_text())
assert len(rows)==19 and all(x['exit']==0 for x in rows)
canon=json.loads((r/'canonical-results.json').read_text())
assert len(canon)==2 and all(x['exit']==0 for x in canon)
print(json.dumps({'tests_patch_sha256':manifest['tests_patch_sha256'],'implementation_patch_sha256':manifest['implementation_patch_sha256'],'runtime_input_count':len(paths),'source_manifest_sha256':sha(r/'final-source-manifest.json'),'strict_cases':len(rows),'canonical_entries':len(canon),'tests':tests},indent=2))
