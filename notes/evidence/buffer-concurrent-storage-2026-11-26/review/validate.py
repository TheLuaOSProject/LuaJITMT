from pathlib import Path
import hashlib,json,os,re,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent;root=Path('/workspaces/lj-lockless')
mode,phase=sys.argv[1:3];tree=p/mode;out=tree/('check-'+phase);out.mkdir(exist_ok=False)
meta=json.loads((tree/'build.json').read_text());assert meta['exit']==0
for rel,sha in meta['source_inputs'].items():assert hashlib.sha256((tree/rel).read_bytes()).hexdigest()==sha,rel
for rel,sha in meta['outputs'].items():assert hashlib.sha256((tree/'src'/rel).read_bytes()).hexdigest()==sha,rel
env=os.environ.copy();env.update(LUA_PATH=str(tree/'src/?.lua')+';'+str(tree/'tests/lib/?.lua')+';;',ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',TSAN_OPTIONS='halt_on_error=1',LUAJIT_STOCK='/tmp/lj-nov-stock.XunoVr/src/luajit')
rows=[];identities={}
def ident(f):
 f=Path(f).resolve();identities[str(f)]=hashlib.sha256(f.read_bytes()).hexdigest();(out/'identities.json').write_text(json.dumps(identities,indent=2)+'\n')
def run(name,cmd,timeout=65,test=True):
 t=time.monotonic()
 with (out/(name+'.stdout')).open('wb') as so,(out/(name+'.stderr')).open('wb') as se:
  try:code=subprocess.run(cmd,cwd=tree,env=env,stdout=so,stderr=se,timeout=timeout).returncode
  except subprocess.TimeoutExpired:code=124
 rows.append(dict(name=name,command=cmd,seconds=time.monotonic()-t,exit=code,test=test))
 (out/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(mode,phase,name,code,flush=True)
 return code
ident(tree/'src/luajit');ident(tree/'src/libluajit.a')
if phase=='matrix':
 ident(tree/'tools/ci/linux_x64_shared.sh');run('linux-x64-shared',['bash','tools/ci/linux_x64_shared.sh'],timeout=300)
elif phase=='smoke':
 script=tree/'tests/t-buffer-thread-safety.lua';ident(script)
 for flag in ['-joff','-jon']:run('buffer'+flag,[str(tree/'src/luajit'),flag,str(script)])
 source=tree/'tests/t-buffer-consume.c';ident(source);exe=out/'consume'
 cmd=[meta['cc'],'-O2','-g','-Wall','-Wextra','-Werror','-mcx16',*meta['xcflags'],*meta['cflags'],'-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(source),str(tree/'src/libluajit.a'),*meta['ldflags'],'-Wl,-E','-Wl,--wrap=lj_str_new','-lm','-ldl','-pthread','-o',str(exe)]
 if run('consume-build',cmd,test=False)==0:ident(exe);run('consume',[str(exe)])
elif phase=='public':
 source=root/'tests/t-buffer-concurrent-public.lua';ident(source)
 for flag in ['-joff','-jon']:run('public'+flag,[str(tree/'src/luajit'),flag,str(source)])
elif phase in ['progress','traverse']:
 source=root/('tests/t-buffer-progress.c' if phase=='progress' else 'tests/t-gc2-traverse.c');ident(source);exe=out/phase
 cmd=[meta['cc'],'-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16',*meta['xcflags'],*meta['cflags'],'-MMD','-MF',str(out/'progress.d'),'-I'+str(tree/'src'),'-I'+str(tree/'tests'),str(source),str(tree/'src/libluajit.a'),*meta['ldflags'],'-Wl,-E','-lm','-ldl','-pthread','-o',str(exe)]
 if run('progress-build',cmd,test=False)==0:ident(exe);run('progress',[str(exe)])
else:raise ValueError(phase)
summary=dict(runtime_pass=sum(r['test'] and r['exit']==0 for r in rows),runtime_fail=sum(r['test'] and r['exit']!=0 for r in rows),build_fail=sum(not r['test'] and r['exit']!=0 for r in rows))
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(mode,phase,summary,flush=True)
sys.exit(any(r['exit']!=0 for r in rows))
