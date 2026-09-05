import pathlib,subprocess,json,time,hashlib,resource,os,shutil
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=pathlib.Path(__file__).resolve().parent;original=pathlib.Path('/tmp/lj-native-owner-root-tail-proof-20260905-zhqsneo2/candidate-assert');fixed=r/'remote-only'
flags=['-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLUA_USE_ASSERT']
rows=[]
def invoke(name,cmd,cwd=None,env=None):
 st=time.monotonic()
 try:
  p=subprocess.run(cmd,cwd=cwd,env=env,capture_output=True,text=True,timeout=25);row={'case':name,'command':cmd,'cwd':str(cwd) if cwd else None,'exit':p.returncode,'seconds':time.monotonic()-st,'stdout':p.stdout,'stderr':p.stderr}
 except subprocess.TimeoutExpired as e:row={'case':name,'command':cmd,'timeout':True,'seconds':time.monotonic()-st,'stdout':str(e.stdout),'stderr':str(e.stderr)}
 rows.append(row);(r/'restricted-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(row),flush=True);return row
for name,t in [('rejected-exact-flags',original),('remote-only',fixed)]:
 exe=r/(name+'-probe')
 cmd=['taskset','-c','0-15','gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(t/'src'),str(r/'t-local-native-duplicate.c'),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-Wl,--wrap=lj_gc2_scan_cycle_owner_tg_roots_native_parked','-Wl,--wrap=lj_lex_gc2_markroots','-o',str(exe)]
 assert invoke('compile-'+name,cmd)['exit']==0
 row=invoke(name,['taskset','-c','0-15',str(exe)])
 assert row.get('exit')==(-6 if name.startswith('rejected') else 0)
exe=r/'remote-progress-probe'
cmd=['taskset','-c','0-15','gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(fixed/'src'),str(r/'review-input/completion-probe.c'),str(fixed/'src/libluajit.a'),'-lm','-ldl','-pthread','-Wl,--wrap=lj_gc2_scan_cycle_owner_tg_roots_native_parked','-Wl,--wrap=lj_gc2_scan_cycle_owner_tg_roots','-Wl,--wrap=lj_gc2_reclaim_retired','-o',str(exe)]
assert invoke('compile-remote-progress',cmd)['exit']==0
for mode in ['0','1']:
 row=invoke('remote-progress-mode'+mode,['taskset','-c','0-15',str(exe),mode]);assert row.get('exit')==0
