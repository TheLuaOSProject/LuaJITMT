from pathlib import Path
import shutil,subprocess,json,os,time
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y')
# Separate mutation control: omit exactly the post-protected-call flag cleanup.
v='negative-flag';shutil.copytree(r/'fix-normal',r/v,symlinks=True,dirs_exist_ok=True)
p=r/v/'src/lj_opt_loop.c';s=p.read_text();old='''  ** carry eligibility into loop retry, another trace, or later optimizer work. */
  J->loop_cdata_fload = 0;
''';assert old in s;s=s.replace(old,'''  ** carry eligibility into loop retry, another trace, or later optimizer work. */
  /* NEGATIVE CONTROL: omit post-call cleanup. */
''');p.write_text(s)
cmd=['taskset','-c','0-15','make','-C',str(r/v/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
p=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
(r/'negative-flag-build.stdout').write_text(p.stdout);(r/'negative-flag-build.stderr').write_text(p.stderr);assert p.returncode==0
exe=r/'negative-flag-fixture';cc=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(r/v/'src'),'-I'+str(r/v/'tests'),str(r/'flag-error.c'),str(r/v/'src/libluajit.a'),'-Wl,--wrap=lj_mem_realloc','-lm','-ldl','-pthread','-o',str(exe)]
p=subprocess.run(cc,capture_output=True,text=True);assert p.returncode==0,p.stderr
env=os.environ.copy();env['LUA_PATH']=str(r/v/'src/?.lua')+';;';run=['taskset','-c','0-15',str(exe)];p=subprocess.run(run,capture_output=True,text=True,env=env,timeout=20)
for k in ['stdout','stderr']:(r/('negative-flag.'+k)).write_text(getattr(p,k))
(r/'negative-flag-result.json').write_text(json.dumps(dict(build=cmd,compile=cc,command=run,exit=p.returncode,stdout=p.stdout,stderr=p.stderr),indent=2)+'\n');print('negativeflag',p.returncode,p.stderr)
# Global worker activation + real collector gate close from the peer path.
s=(r/'phase-gate.c').read_text().replace(' assert(gc2_phase_acq(testg)==LJ_GC2_MARK);\n la_store32_rel(&saw_native,1);la_store32_rel(&requested,1);\n lj_gc2_jit_mark_request_exit(testg);',''' assert(gc2_phase_acq(testg)==LJ_GC2_IDLE);
 la_store32_rel(&saw_native,1);
 assert(lj_gc2_workers_set(testg,1)==1);
 la_store32_rel(&requested,1);
 lj_gc2_mark_begin(testg);''')
s=s.replace(''' lj_gc2_mark_begin(testg);
 assert(gc2_phase_acq(testg)==LJ_GC2_MARK);
 assert(lj_gc2_jit_entry_open(testg));''',''' assert(gc2_phase_acq(testg)==LJ_GC2_IDLE);
 assert(lj_gc2_jit_entry_open(testg));''')
s=s.replace(''' lua_close(L);return 0;''',''' assert(lj_gc2_workers_set(testg,0)==1);
 lua_close(L);return 0;''')
s=s.replace('phase-gate early-exit','global-worker early-exit')
(r/'global-worker.c').write_text(s)
