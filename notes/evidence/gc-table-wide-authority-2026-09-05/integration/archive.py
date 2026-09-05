from pathlib import Path
import subprocess, hashlib, difflib, json
P=Path(__file__).resolve().parent;T=P/'tree';base='ff2a6ca0260c0b0c55e2fa708e4f603fd9d4070f'
source=['src/lj_arena.c','src/lj_arena.h','src/lj_gc2.c','src/lj_gc2.h']
tests=['tests/lib/gc2_wide_fixture_helpers.h','tests/lib/gc2_wide_reuse_helpers.h','tests/t-arena-huge-tail.c','tests/t-gc2-sweep-table-coalescing.c','tests/t-gc2-traverse.c','tests/t-x64-tnew-empty-inline.c','tests/t-jit-fnew-bump.c','tests/suites/m2_arena.lua','tests/suites/m3_gc.lua','tests/suites/m5_x64.lua']
sha=lambda b:hashlib.sha256(b).hexdigest()
patches={};rows=[];originals={}
for group,files in [('source',source),('tests-integration',tests)]:
 patch=''
 for f in files:
  r=subprocess.run(['git','show',base+':'+f],cwd='/workspaces/lj-lockless',capture_output=True)
  old=r.stdout if r.returncode==0 else b'';new=(T/f).read_bytes();originals[f]=old
  patch+=''.join(difflib.unified_diff(old.decode().splitlines(True),new.decode().splitlines(True),fromfile='a/'+f if r.returncode==0 else '/dev/null',tofile='b/'+f))
  rows.append({'path':f,'new_file':r.returncode!=0,'sha256':sha(new),'group':group})
 patches[group]=patch;(P/(group+'.patch')).write_text(patch)
patches['source-and-tests']=patches['source']+patches['tests-integration']
(P/'source-and-tests.patch').write_text(patches['source-and-tests'])
check=P/'patch-check';check.mkdir(exist_ok=True)
for f,b in originals.items():
 if b:
  dst=check/f;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(b)
cmd=['git','apply','--check',str(P/'source-and-tests.patch')];r=subprocess.run(cmd,cwd=check,capture_output=True,text=True);assert r.returncode==0,r.stderr
manifest={'base':base,'files':rows,'patches':{k:sha(v.encode()) for k,v in patches.items()},'patch_check':{'command':cmd,'cwd':str(check),'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}}
(P/'source-test-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest['patches'],indent=2))
