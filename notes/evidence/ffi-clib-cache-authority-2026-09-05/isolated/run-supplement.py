from pathlib import Path
import hashlib,json,os,subprocess,sys,time,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0));p=Path(__file__).resolve().parent
variant,suite=sys.argv[1:3]
tree=Path('/tmp/lj-udata-pure-receiver-combined-20260905-sn9vd57b/candidate') if variant=='exact' else Path('/tmp/lj-clib-cache-root-20260905-i59mqoic/v2')/variant
out=p/('supplement-'+variant+'-'+suite+'-results.json');assert not out.exists(),out
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest();rows=[]
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;';delta={'LUA_PATH':env['LUA_PATH']}
if variant=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1';delta['ASAN_OPTIONS']=env['ASAN_OPTIONS']
def run(cmd,meta={},timeout=30):
 start=time.monotonic();inputs={str(f):sha(f) for f in [p/'run-supplement.py',p/'t-clib-cache-authority.lua',p/'t-clib-cache-supplement.lua',p/'t-clib-cache-geometry.c',p/'authority.so',tree/'src/luajit',tree/'src/jit/vmdef.lua']}
 module=p/('geometry-'+variant+'.so')
 if module.exists():inputs[str(module)]=sha(module)
 try:
  r=subprocess.run(cmd,cwd=p,env=env,capture_output=True,text=True,timeout=timeout);res={'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
 except subprocess.TimeoutExpired as e:
  dec=lambda v:v.decode(errors='replace') if isinstance(v,bytes) else v or ''
  res={'exit':None,'timeout':True,'stdout':dec(e.stdout),'stderr':dec(e.stderr)}
 rows.append({'command':cmd,'cwd':str(p),'environment':delta,'cpu_affinity':sorted(os.sched_getaffinity(0)),'seconds':time.monotonic()-start,'inputs':inputs,**meta,**res});out.write_text(json.dumps(rows,indent=2)+'\n')
 if res['exit']!=0:print(variant,suite,meta,res,flush=True)
 return res
if suite=='compile':
 flags=['-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS'] if variant in ['strict','asan'] else []
 module=p/('geometry-'+variant+'.so');deps=p/('geometry-'+variant+'.d')
 cmd=['clang' if variant=='asan' else 'cc','-std=gnu11','-shared','-fPIC','-O1' if variant=='asan' else '-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags
 if variant=='asan':cmd+=['-fsanitize=address','-fno-omit-frame-pointer']
 cmd+=['-MMD','-MF',str(deps),'-I'+str(tree/'src'),str(p/'t-clib-cache-geometry.c'),'-o',str(module)]
 r=run(cmd,{'operation':'compile'},60)
 if r['exit']==0:
  rows[-1]['output_binary_sha256']=sha(module);rows[-1]['header_and_source_dependencies']={f:sha(f) for f in deps.read_text().replace('\\\n',' ').split(': ',1)[1].split()};out.write_text(json.dumps(rows,indent=2)+'\n')
 if r['exit']!=0:sys.exit(1)
elif suite=='authority':
 cases={'function':['false','nil','other','fenv','close'],'read':['false','nil','other','close'],'write':['false','nil','other','close'],'zero':['negative-zero','positive-zero','false','nil'],'big':['number','false','nil']}
 for kind,modes in cases.items():
  for mode in modes:
   for target in ['root','side']:
    for status in ['-joff','-jon']:
     cmd=[str(tree/'src/luajit'),status,str(p/'t-clib-cache-authority.lua'),kind,mode,target,str(p/'authority.so'),'no-helper']
     run(cmd,{'kind':kind,'mode':mode,'target':target,'status':status})
else:
 assert (p/('geometry-'+variant+'.so')).is_file(),'missing compiled geometry observer'
 cases={'zero':['nan','pos-inf','neg-inf'],'read':['resize-other'],'write':['resize-other']}
 if suite=='pilot':cases={'zero':['nan'],'write':['resize-other']}
 for kind,modes in cases.items():
  for mode in modes:
   for target in ['root','side']:
    for phase in ['pre-mt','mt']:
     for status in ['-jon'] if suite=='pilot' else ['-joff','-jon']:
      cmd=[str(tree/'src/luajit'),status,str(p/'t-clib-cache-supplement.lua'),kind,mode,target,str(p/'authority.so'),'helper' if phase=='mt' and variant!='exact' else 'no-helper','gc',phase,str(p/('geometry-'+variant+'.so'))]
      run(cmd,{'kind':kind,'mode':mode,'target':target,'phase':phase,'status':status})
print(variant,suite,'passes',sum(r['exit']==0 for r in rows),'of',len(rows),flush=True)
