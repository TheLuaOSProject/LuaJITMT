from pathlib import Path
import json,hashlib,difflib,shutil,subprocess
p=Path(__file__).parent;repo=Path('/workspaces/lj-lockless');sha=lambda b:hashlib.sha256(b).hexdigest()
original=(p/'original-t-jit-xsave.c').read_text();final=(p/'settled-v2-t-jit-xsave.c').read_text()
patch=''.join(difflib.unified_diff(original.splitlines(keepends=True),final.splitlines(keepends=True),fromfile='a/tests/t-jit-xsave.c',tofile='b/tests/t-jit-xsave.c'))
(p/'fixture-only.patch').write_text(patch)
functions=['assert_xsave_retf_shape','assert_xsave_trace_shape','test_xsave_native_owner_lifecycle','test_certified_native_frame_scanner']
def function(s,name):
 start=s.index('static void '+name+'(');brace=s.index('{',start);depth=0
 for i in range(brace,len(s)):
  if s[i]=='{':depth+=1
  elif s[i]=='}':
   depth-=1
   if depth==0:return s[start:i+1]
rows=[]
for name in functions:
 a=function(original,name);b=function(final,name);assert a==b
 rows.append({'function':name,'unchanged':True,'sha256':sha(a.encode())})
(p/'assertion-preservation.json').write_text(json.dumps(rows,indent=2)+'\n')
identity={'review_base':'b4e26564542cb8bfa997a11c6a90e5e0017a2f79','current_base':'8d342cd6456d2f93ba07a779cdde30d4806eb90f','source_comparisons':{},'build_artifacts':[]}
origin=Path('/tmp/lj-callxs-callback-geometry-20260905-vgzqdmkx')
variants=[('b4',origin/'base-assert',identity['review_base']),('b4-callback',origin/'fix-assert',identity['review_base']),('b4-callback-asan',origin/'fix-asan',identity['review_base']),('current',p/'current-assert',identity['current_base']),('canonical-restored-normal',p/'canonical',identity['current_base'])]
for name,tree,base in variants:
 files=subprocess.check_output(['git','ls-tree','-r','--name-only',base,'src'],cwd=repo,text=True).splitlines();different=[];source=[]
 for f in files:
  expected=subprocess.check_output(['git','show',base+':'+f],cwd=repo);actual=(tree/f).read_bytes();source.append({'path':f,'sha256':sha(actual),'base_sha256':sha(expected)})
  if actual!=expected:different.append(f)
 identity['source_comparisons'][name]={'base':base,'tracked_source_count':len(files),'differences':different,'sources':source}
 for f in ['src/libluajit.a','src/luajit','src/lj_asm.o','src/lj_ccall.o','src/lj_ffrecord.o','src/lj_trace.o','src/lj_vm.o']:
  x=tree/f
  if x.exists():identity['build_artifacts'].append({'path':str(x),'bytes':x.stat().st_size,'sha256':sha(x.read_bytes())})
for x in [p/'current-final-xsave',p/'canonical-tmp/lj_t-jit-xsave',origin/'base-assert-jit-xsave']:
 if x.exists():identity['build_artifacts'].append({'path':str(x),'bytes':x.stat().st_size,'sha256':sha(x.read_bytes())})
(p/'source-and-build-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
r=p/'reconstruct';(r/'tests').mkdir(parents=True)
(r/'tests/t-jit-xsave.c').write_text(original)
cmd=['patch','--batch','--fuzz=0','-p1','-i',str(p/'fixture-only.patch')];q=subprocess.run(cmd,cwd=r,text=True,capture_output=True);assert q.returncode==0 and (r/'tests/t-jit-xsave.c').read_bytes()==final.encode()
(p/'fixture-patch-verification.json').write_text(json.dumps({'command':cmd,'cwd':str(r),'exit':q.returncode,'stdout':q.stdout,'stderr':q.stderr,'matches_final':True},indent=2)+'\n')
for f in ['src/lj_trace.c','src/lj_ffrecord.c','src/lj_asm.c','src/lj_asm_x86.h','src/lj_ccall.c','tests/suites/m6_jit.lua']:
 d=p/'source-input'/f;d.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(origin/'base-assert'/f,d)
print('fixture patch',sha(patch.encode()))
print('fixture',sha(final.encode()))
print('canonical binary',sha((p/'canonical-tmp/lj_t-jit-xsave').read_bytes()))
