from pathlib import Path
import hashlib, json, os, shutil, subprocess, time

p=Path(__file__).resolve().parent
tree=p/'optimized'
shutil.copytree(p/'base',tree)
setup=json.loads((p/'setup.json').read_text())
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
for rel in setup['changed_runtime_inputs']:
    shutil.copyfile(p/'candidate'/rel,tree/rel)
for rel,digest in setup['combined_inputs'].items():
    assert sha(tree/rel)==digest,rel
flags=['-DLJ_GC2_TEST_HELPERS']
cmd=['taskset','-c','16-19','make','-C',str(tree/'src'),'-j4',
     'BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:',
     'XCFLAGS='+' '.join(flags)]
env=os.environ.copy()
env.pop('ASAN_OPTIONS',None)
start=time.monotonic()
result=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=240)
(p/'optimized-build.json').write_text(json.dumps(dict(command=cmd,cwd=str(tree),flags=flags,
    seconds=time.monotonic()-start,exit=result.returncode,stdout=result.stdout,stderr=result.stderr),indent=2)+'\n')
assert result.returncode==0,result.stderr
(p/'optimized-binaries.json').write_text(json.dumps({f:sha(tree/'src'/f)
    for f in ['luajit','libluajit.a']},indent=2)+'\n')
for rel,digest in setup['combined_inputs'].items():
    assert sha(tree/rel)==digest,rel
print('optimized built',flush=True)
