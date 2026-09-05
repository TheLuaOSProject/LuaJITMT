from pathlib import Path
import subprocess,os,json,hashlib
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');results=[]
for v in ['base-normal','fix-normal','fix-assert']:
 exe=r/(v+'-phase-gate');tree=r/v
 cc=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(r/'phase-gate.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if v=='fix-assert':cc[1:1]=['-DLUA_USE_ASSERT','-DLJ_GC2_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS']
 p=subprocess.run(cc,capture_output=True,text=True);(r/(v+'-phase-build.stderr')).write_text(p.stderr)
 if p.returncode:print(v,'buildFAIL',p.stderr);results.append(dict(variant=v,compile=cc,exit=p.returncode));continue
 env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;'
 cmd=['taskset','-c','0-15',str(exe)]
 try:p=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=20);code=p.returncode;out,err=p.stdout,p.stderr
 except subprocess.TimeoutExpired as e:code='TIMEOUT';out=(e.stdout or b'').decode();err=(e.stderr or b'').decode()
 for k,s in [('stdout',out),('stderr',err)]:(r/(v+'-phase-gate.'+k)).write_text(s)
 results.append(dict(variant=v,compile=cc,command=cmd,env={'LUA_PATH':env['LUA_PATH']},exit=code,stdout=out,stderr=err))
 print(v,code,out,err,flush=True)
(r/'phase-results.json').write_text(json.dumps(results,indent=2)+'\n')
