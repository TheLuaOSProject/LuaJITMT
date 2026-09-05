from pathlib import Path
import subprocess,json,time,hashlib,os
p=Path(__file__).parent
src=Path('/tmp/lj-callxs-callback-geometry-20260905-vgzqdmkx')
s=(p/'settled-v2-t-jit-xsave.c').read_text()
(p/'disabled-producer-t-jit-xsave.c').write_text(s.replace('    "for i = 1, 40 do assert(stage(80) == 3240) end\\n"','    "jit.off(stage, true)\\n"\n    "for i = 1, 40 do assert(stage(80) == 3240) end\\n"'))
(p/'no-collection-t-jit-xsave.c').write_text(s.replace('    "collectgarbage(\'collect\')\\n"\n',''))
rows=[]
for name,tree,fixture,asan in [('final-b4','base-assert','settled-v2-t-jit-xsave.c',False),('final-callback','fix-assert','settled-v2-t-jit-xsave.c',False),('final-callback-asan','fix-asan','settled-v2-t-jit-xsave.c',True),('negative-no-producer','base-assert','disabled-producer-t-jit-xsave.c',False),('negative-no-collection','base-assert','no-collection-t-jit-xsave.c',False)]:
 tree=src/tree;exe=p/name
 cmd=['clang' if asan else 'cc','-std=gnu11','-O1' if asan else '-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-DLUA_USE_ASSERT','-DLJ_XSAVE_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS']
 if asan:cmd+=['-fsanitize=address','-fno-omit-frame-pointer']
 cmd+=['-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(p/fixture),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 t=time.monotonic();a=subprocess.run(cmd,text=True,capture_output=True);r={'name':name,'compile':cmd,'compile_exit':a.returncode,'compile_stdout':a.stdout,'compile_stderr':a.stderr,'fixture_sha256':hashlib.sha256((p/fixture).read_bytes()).hexdigest(),'archive_sha256':hashlib.sha256((tree/'src/libluajit.a').read_bytes()).hexdigest()}
 if a.returncode==0:
  cmd=['taskset','-c','0-15',str(exe)];env=os.environ.copy();overrides={'LUA_PATH':str(tree/'src/?.lua')+';;'}
  if asan:overrides['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
  env.update(overrides)
  try:b=subprocess.run(cmd,cwd=tree,text=True,capture_output=True,env=env,timeout=30);r.update(command=cmd,cwd=str(tree),env_override=overrides,exit=b.returncode,stdout=b.stdout,stderr=b.stderr)
  except subprocess.TimeoutExpired as e:r.update(command=cmd,exit='timeout30',stdout=str(e.stdout),stderr=str(e.stderr))
  r['executable_sha256']=hashlib.sha256(exe.read_bytes()).hexdigest()
 r['seconds']=time.monotonic()-t;rows.append(r);(p/'settled-validation.json').write_text(json.dumps(rows,indent=2)+'\n')
 print(name,r.get('exit',r['compile_exit']),r.get('stdout','').strip(),r.get('stderr','').strip(),flush=True)
