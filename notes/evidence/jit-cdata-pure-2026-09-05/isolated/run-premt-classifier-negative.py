from pathlib import Path
import subprocess,shutil,os,json,hashlib
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');v='negative-classifier';tree=r/v;shutil.copytree(r/'fix-normal',tree,symlinks=True,dirs_exist_ok=True)
p=tree/'src/lj_opt_loop.c';s=p.read_text();old='J->loop_cdata_fload = !needs_poll && loop_cdata_fload_pure(J, invar);';assert old in s;s=s.replace(old,'J->loop_cdata_fload = !needs_poll;  /* NEGATIVE: skip complete-body proof. */\n    (void)loop_cdata_fload_pure(J, invar);');p.write_text(s)
build=['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:'];z=subprocess.run(build,capture_output=True,text=True,timeout=60);assert z.returncode==0,z.stderr
for k in ['stdout','stderr']:(r/('negative-classifier-build.'+k)).write_text(getattr(z,k))
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;';cmd=['taskset','-c','0-15',str(tree/'src/luajit'),str(r/'eligibility.lua'),'allocate'];z=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=20)
for k in ['stdout','stderr']:(r/('negative-classifier.'+k)).write_text(getattr(z,k))
(r/'negative-classifier-result.json').write_text(json.dumps(dict(build=build,command=cmd,source_sha256=hashlib.sha256(p.read_bytes()).hexdigest(),exit=z.returncode,stdout=z.stdout,stderr=z.stderr),indent=2)+'\n');print(z.returncode,z.stdout,z.stderr)
