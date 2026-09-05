from pathlib import Path
import sys,subprocess,os,time,json,hashlib
r=Path(__file__).resolve().parent
v=sys.argv[1] if len(sys.argv)>1 else 'helpers'
tree=r/v
s=tree/'src'
label=v+'-safety'
res=[]
env=dict(os.environ,LUA_PATH=str(s/'?.lua')+';;')
if v=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
flags=['-DLUA_USE_ASSERT','-DLUA_USE_APICHECK'] if v!='candidate' and v!='control' else []
if v in ('helpers','controlhelpers'):flags+=['-DLJ_GC2_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS']
if v=='asan':flags+=['-fsanitize=address','-fno-omit-frame-pointer']
def run(cmd,name,limit=60):
 st=time.monotonic()
 with (r/(label+'-'+name+'.stdout')).open('w') as out,(r/(label+'-'+name+'.stderr')).open('w') as err:
  try: x=subprocess.run(cmd,cwd=tree,env=env,stdout=out,stderr=err,timeout=limit);code=x.returncode
  except subprocess.TimeoutExpired:code='timeout'
 row={'argv':cmd,'name':name,'cwd':str(tree),'LUA_PATH':env['LUA_PATH'],'ASAN_OPTIONS':env.get('ASAN_OPTIONS'),'exit_code':code,'seconds':time.monotonic()-st,'stdout':label+'-'+name+'.stdout','stderr':label+'-'+name+'.stderr'}
 res.append(row);(r/(label+'-results.json')).write_text(json.dumps(res,indent=2)+'\n');print(json.dumps({'name':name,'exit_code':code,'seconds':row['seconds']}),flush=True)
 return code==0
specs=[
 ('t-gc2-pacing-atomic',[],[[]]),
 ('t-gc2-interp-hard-check',[],[[]]),
 ('t-gc2-jit-hard-check',[],[[]]),
 ('t-gc2-alloc-account',[],[[]]),
 ('t-gc2-activation-veto',[],[[]]),
 ('t-gc2-worker-scheduler',['pthread_create','pthread_join'],[[]]),
 ('t-gc2-mark-close-progress',['lj_native_enter','lj_thr_retry_yield'],[[]]),
 ('t-gc2-jit-mark-coop',[],[[]]),
 ('t-gc2-jit-sweep-coop',[],[[]]),
 ('t-jit-idle-reclaim-entry',[],[[]]),
 ('t-gc-root-pending-race',[],[[]]),
 ('t-udata-construction-roots',[],[[]]),
 ('t-func-construction-anchor',[],[[]]),
 ('t-threading-spawn-native',['pthread_create','lj_vm_cpcall','lj_tg_fini_thread'],[[]]),
 ('t-safepoint-native-root-hold',['lj_gc2_scan_cycle_owner_tg_roots_native_parked','lj_gc2_scan_cycle_global_roots','lj_gc2_flush_ssb'],[[str(i)] for i in range(4)]),
 ('t-safepoint-remote-root-completion',['lj_gc2_scan_cycle_owner_tg_roots_native_parked'],[[str(i)] for i in range(6)]),
 ('t-safepoint-local-native-duplicate',['lj_gc2_scan_cycle_owner_tg_roots_native_parked','lj_lex_gc2_markroots'],[[]]),
]
chosen=set(sys.argv[2:])
for name,wraps,modes in specs:
 if chosen and name not in chosen:continue
 exe=r/(label+'-'+name)
 cmd=['clang' if v=='asan' else 'cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-I'+str(s),*flags,str(tree/'tests'/(name+'.c')),str(s/'libluajit.a'),'-Wl,-E','-lm','-ldl','-pthread',*['-Wl,--wrap='+w for w in wraps],'-o',str(exe)]
 if not run(cmd,name+'-compile'):continue
 for mode in modes:run([str(exe),*mode],name+('-'+mode[0] if mode else ''),60)
for name in ['t-gc-active-thread-roots','t-gc-workers','t-gc2-finalizer-peer-collect']:
 if chosen and name not in chosen:continue
 for off in [True,False]:run([str(s/'luajit'),*(['-joff'] if off else []),str(tree/'tests'/(name+'.lua'))],name+('-joff' if off else '-jit'),60)
paths=[r/row['stdout'] for row in res]+[r/row['stderr'] for row in res]+[s/'libluajit.a']
for row in res:
 p=Path(row['argv'][0]);
 if p.is_file() and p.is_relative_to(r):paths.append(p)
(r/(label+'-identities.json')).write_text(json.dumps({str(p.relative_to(r)):{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for p in paths},indent=2)+'\n')
raise SystemExit(0 if all(row['exit_code']==0 for row in res) else 1)
