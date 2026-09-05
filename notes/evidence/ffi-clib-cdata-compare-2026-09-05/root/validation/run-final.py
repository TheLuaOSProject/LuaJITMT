from pathlib import Path
import hashlib,json,os,subprocess,sys,time,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
root=p.parent
variant=sys.argv[1];suite=sys.argv[2]
tree=p/'baseline' if variant=='baseline' else root/variant
out=p/('final-'+variant+'-'+suite+'-results.json')
assert not out.exists(),out
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
flags=['-DLUA_USE_APICHECK','-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS']
env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;';delta={'LUA_PATH':env['LUA_PATH']}
if variant=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1';delta['ASAN_OPTIONS']=env['ASAN_OPTIONS']
rows=[]
def run(cmd,meta={},timeout=30):
 start=time.monotonic()
 inputs={str(f):sha(f) for f in [p/'run-final.py',p/'t-clib-cache-authority.lua',p/'t-clib-cache-between-close.c',p/'t-clib-cache-authority-lib.c',p/'authority.so',tree/'src/luajit',tree/'src/libluajit.a',tree/'src/jit/vmdef.lua']}
 if Path(cmd[0]).is_file():inputs[cmd[0]]=sha(cmd[0])
 try:
  r=subprocess.run(cmd,cwd=p,env=env,capture_output=True,text=True,timeout=timeout);res={'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
 except subprocess.TimeoutExpired as e:
  def dec(v):return v.decode(errors='replace') if isinstance(v,bytes) else v or ''
  res={'exit':None,'timeout':True,'stdout':dec(e.stdout),'stderr':dec(e.stderr)}
 rows.append({'command':cmd,'cwd':str(p),'environment':delta,'seconds':time.monotonic()-start,'cpu_affinity':sorted(os.sched_getaffinity(0)),'inputs':inputs,**meta,**res})
 out.write_text(json.dumps(rows,indent=2)+'\n')
 if res['exit']!=0:print(variant,suite,meta,res,flush=True)
 return res
if suite=='compile':
 cc='clang' if variant=='asan' else 'cc'
 cmd=[cc,'-std=gnu11','-O1' if variant=='asan' else '-O2','-g','-Wall','-Wextra','-Werror','-mcx16']
 if variant in ['strict','asan']:cmd+=flags
 if variant=='asan':cmd+=['-fsanitize=address','-fno-omit-frame-pointer']
 binary=p/('final-'+variant+'-between-close')
 cmd+=['-MMD','-MF',str(p/('final-'+variant+'-between-close.d')),'-I'+str(tree/'src'),str(p/'t-clib-cache-between-close.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-Wl,--wrap=lj_tab_cmpcdata_kgc_rooted_try','-o',str(binary)]
 r=run(cmd,{'operation':'compile'},60)
 if r['exit']==0:
  rows[-1]['output_binary_sha256']=sha(binary)
  deps=(p/('final-'+variant+'-between-close.d')).read_text().replace('\\\n',' ').split(': ',1)[1].split()
  rows[-1]['header_and_source_dependencies']={f:sha(f) for f in deps}
  out.write_text(json.dumps(rows,indent=2)+'\n')
  print(variant,'wrapper built',sha(binary),flush=True)
elif suite in ['wrapper','gc-wrapper']:
 for kind in ['read','write']:
  for target in ['root','side']:
   cmd=[str(p/('final-'+variant+'-between-close')),str(p/'t-clib-cache-authority.lua'),kind,'between-close',target,str(p/'authority.so')]
   if suite=='gc-wrapper':cmd+=['gc']
   run(cmd,{'kind':kind,'mode':'between-close','target':target,'gc':suite=='gc-wrapper','status':'-jon'})
else:
 cases={'function':['false','nil','other','fenv','close'],'read':['false','nil','other','close'],'write':['false','nil','other','close'],'zero':['negative-zero','positive-zero','false','nil'],'big':['number','false','nil']}
 if suite in ['gc','gc-pilot']:cases={'function':['nil','other','fenv'],'read':['close'],'write':['close']}
 if suite=='gc-pilot':cases={'function':['fenv']}
 for kind,modes in cases.items():
  for mode in modes:
   for target in ['root','side']:
    for status in ['-joff','-jon'] if suite!='gc-pilot' else ['-jon']:
     cmd=[str(tree/'src/luajit'),status,str(p/'t-clib-cache-authority.lua'),kind,mode,target,str(p/'authority.so'),'helper' if variant!='baseline' else 'no-helper']
     if suite in ['gc','gc-pilot']:cmd+=['gc']
     run(cmd,{'kind':kind,'mode':mode,'target':target,'gc':suite in ['gc','gc-pilot'],'status':status})
  print(variant,suite,kind,'complete',flush=True)
print(variant,suite,'passes',sum(r['exit']==0 for r in rows),'of',len(rows),flush=True)
