from pathlib import Path
import subprocess,json,time,hashlib,os,tarfile,io,shutil
p=Path(__file__).parent;repo=Path('/workspaces/lj-lockless');base='8d342cd6456d2f93ba07a779cdde30d4806eb90f';rows=[]
raw=subprocess.check_output(['git','archive',base,'Makefile','.relver','src','dynasm','tests','tools','COPYRIGHT'],cwd=repo)
for name in ('current-assert','canonical'):
 tree=p/name;tree.mkdir()
 with tarfile.open(fileobj=io.BytesIO(raw)) as t:t.extractall(tree)
 assert (tree/'tests/t-jit-xsave.c').read_bytes()==(p/'original-t-jit-xsave.c').read_bytes()
 cmd=['patch','--batch','--fuzz=0','-p1','-i',str(p/'candidate.patch')];r=subprocess.run(cmd,cwd=tree,text=True,capture_output=True);assert r.returncode==0,(r.stdout,r.stderr)
 shutil.copyfile(p/'settled-v2-t-jit-xsave.c',tree/'tests/t-jit-xsave.c')
rows.append({'base':base,'archive_command':['git','archive',base,'Makefile','.relver','src','dynasm','tests','tools','COPYRIGHT'],'callback_patch_sha256':hashlib.sha256((p/'candidate.patch').read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256((p/'settled-v2-t-jit-xsave.c').read_bytes()).hexdigest()})
def run(name,cmd,cwd,env_overrides=None,timeout=90):
 env=os.environ.copy();env.update(env_overrides or {});t=time.monotonic();r={'name':name,'command':cmd,'cwd':str(cwd),'env_override':env_overrides or {}}
 with (p/(name+'.stdout')).open('w') as out,(p/(name+'.stderr')).open('w') as err:
  try:x=subprocess.run(cmd,cwd=cwd,env=env,stdout=out,stderr=err,timeout=timeout);r['exit']=x.returncode
  except subprocess.TimeoutExpired:r['exit']='timeout'+str(timeout)
 r['seconds']=time.monotonic()-t;r['stdout']=str(p/(name+'.stdout'));r['stderr']=str(p/(name+'.stderr'));rows.append(r);(p/'current-validation.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,r['exit'],r['seconds'],flush=True);return r['exit']
tree=p/'current-assert';flags='-DLUA_USE_ASSERT -DLJ_XSAVE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS'
assert run('current-build',['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:', 'XCFLAGS='+flags],tree)==0
cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags.split()+['-I'+str(tree/'src'),str(tree/'tests/t-jit-xsave.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(p/'current-final-xsave')]
assert run('current-compile',cmd,tree)==0
assert run('current-final',['taskset','-c','0-15',str(p/'current-final-xsave')],tree,{'LUA_PATH':str(tree/'src/?.lua')+';;'},30)==0
identity=[]
for f in sorted((tree/'src').rglob('*')):
 if f.is_file() and (f.suffix in ('.c','.h','.dasc','.S','.o','.a') or f.name in ('Makefile','luajit')):
  identity.append({'path':str(f),'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()})
identity.append({'path':str(p/'current-final-xsave'),'bytes':(p/'current-final-xsave').stat().st_size,'sha256':hashlib.sha256((p/'current-final-xsave').read_bytes()).hexdigest()})
(p/'current-source-build-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
tree=p/'canonical';tmp=p/'canonical-tmp';tmp.mkdir();env={'LJ_TEST_ROOT':str(tree),'JOBS':'4','MAKE_JOBS':'4','TMPDIR':str(tmp),'LUA_PATH':str(tree/'src/?.lua')+';;'}
runner='/tmp/lj-callxs-callback-geometry-20260905-vgzqdmkx/base-normal/src/luajit'
assert run('canonical-final',['taskset','-c','0-15',runner,str(tree/'tools/test.lua'),'m6_jit_xsave'],tree,env,90)==0
