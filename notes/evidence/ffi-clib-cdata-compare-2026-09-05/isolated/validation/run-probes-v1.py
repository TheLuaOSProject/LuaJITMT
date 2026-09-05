from pathlib import Path
import hashlib,json,os,subprocess,sys,time
p=Path(__file__).resolve().parent
variant,suite=sys.argv[1:3];tree=p.parent/variant
out=p/('probe-v1-'+variant+'-'+suite+'-results.json');assert not out.exists(),out
flags=['-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS'] if variant in ['strict','asan'] else []
envadd={'LUA_PATH':str(tree/'src/?.lua')+';;'}
if variant=='asan':envadd['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest();rows=[]
def run(cmd,meta={},timeout=38):
 start=time.monotonic()
 inputs={str(f):sha(f) for f in [p/'t-clib-cdata-probe.c',p/'t-clib-cdata-probe.lua',p/'authority.so',tree/'src/luajit',tree/'src/libluajit.a',tree/'src/jit/vmdef.lua']}
 if Path(cmd[0]).is_file():inputs[cmd[0]]=sha(cmd[0])
 try:
  r=subprocess.run(cmd,cwd=p,env={**os.environ,**envadd},capture_output=True,text=True,timeout=timeout);result={'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
 except subprocess.TimeoutExpired as e:
  dec=lambda v:v.decode(errors='replace') if isinstance(v,bytes) else v or ''
  result={'exit':None,'timeout':True,'stdout':dec(e.stdout),'stderr':dec(e.stderr)}
 row={'command':cmd,'cwd':str(p),'environment':envadd,'seconds':time.monotonic()-start,'inputs':inputs,**meta,**result};rows.append(row);out.write_text(json.dumps(rows,indent=2)+'\n')
 print(variant,suite,meta,result,flush=True);return row
binary=p/('comparison-probe-'+variant)
if suite=='compile':
 cmd=['clang' if variant=='asan' else 'cc','-std=gnu11','-O1' if variant=='asan' else '-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags
 if variant=='asan':cmd+=['-fsanitize=address','-fno-omit-frame-pointer']
 cmd+=['-MMD','-MF',str(p/('comparison-probe-'+variant+'.d')),'-I'+str(tree/'src'),str(p/'t-clib-cdata-probe.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-Wl,--export-dynamic','-Wl,--wrap=lj_tab_cmpcdata_kgc_rooted_try','-Wl,--wrap=lj_tg_any_jit_active','-o',str(binary)]
 row=run(cmd,{'operation':'compile'},65)
 if row['exit']==0:
  row['output_binary_sha256']=sha(binary)
  deps=(p/('comparison-probe-'+variant+'.d')).read_text().replace('\\\n',' ').split(': ',1)[1].split()
  row['dependencies']={name:sha(name) for name in deps};out.write_text(json.dumps(rows,indent=2)+'\n')
else:
 modes=['wrong','null','smr','gc','flush']
 if variant!='candidate':modes+=['table-refusal','key-refusal']
 kinds=['function'] if suite=='pilot' else ['function','read','write']
 targets=['root'] if suite=='pilot' else ['root','side']
 for mode in modes:
  for kind in kinds:
   for target in targets:
    cmd=[str(binary),str(p/'t-clib-cdata-probe.lua'),kind,mode,target,str(p/'authority.so')]
    if mode in ['gc','flush']:cmd+=['gc']
    run(cmd,{'mode':mode,'kind':kind,'target':target})
assert all(r['exit']==0 for r in rows)
