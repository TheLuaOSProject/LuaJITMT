from pathlib import Path
import subprocess,os,time,json,hashlib
r=Path(__file__).resolve().parent;tree=r/'canonical';tmp=r/'final-canonical-tmp';tmp.mkdir();env=os.environ.copy();overrides={'LJ_TEST_ROOT':str(tree),'JOBS':'4','TMPDIR':str(tmp),'LJ_TEST_DISABLE_BUILD_CACHE':'1'};env.update(overrides)
cmd=['taskset','-c','0-15',str(r/'base-normal/src/luajit'),'-joff',str(tree/'tools/test.lua'),'m6_jit_cdata_basemt_guards'];t=time.monotonic();p=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=120)
(r/'final-canonical.stdout').write_text(p.stdout);(r/'final-canonical.stderr').write_text(p.stderr)
(r/'final-canonical-result.json').write_text(json.dumps({'command':cmd,'cwd':str(tree),'environment_overrides':overrides,'exit':p.returncode,'seconds':time.monotonic()-t,'source_sha256':hashlib.sha256((tree/'src/lj_record.c').read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256((tree/'tests/t-jit-cdata-basemt-guards.lua').read_bytes()).hexdigest(),'suite_sha256':hashlib.sha256((tree/'tests/suites/m6_jit.lua').read_bytes()).hexdigest(),'runtime_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest()},indent=2)+'\n')
print(p.returncode,round(time.monotonic()-t,3),p.stdout[-2500:],p.stderr,flush=True)
