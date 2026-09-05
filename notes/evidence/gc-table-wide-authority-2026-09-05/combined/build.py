from pathlib import Path
import os, subprocess, signal, time, json, sys
P=Path(__file__).resolve().parent
variant=sys.argv[1];T=P/variant
helpers='-DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT'
cmd=['taskset','-c','0-15','make','-C',str(T/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
if variant!='normal':cmd+=['XCFLAGS='+helpers]
if variant=='asan':cmd+=['CC=clang','TARGET_CFLAGS=-O1 -fsanitize=address -fno-omit-frame-pointer','TARGET_LDFLAGS=-fsanitize=address']
env=os.environ.copy();env.pop('ASAN_OPTIONS',None)
start=time.monotonic()
with (P/('build-'+variant+'.stdout')).open('w') as out,(P/('build-'+variant+'.stderr')).open('w') as err:
 q=subprocess.Popen(cmd,env=env,stdout=out,stderr=err,start_new_session=True)
 try:code=q.wait(timeout=180);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);code=q.wait();status='timeout'
r={'command':cmd,'exit':code,'status':status,'seconds':time.monotonic()-start,'ASAN_OPTIONS':'unset; host generators uninstrumented','target_only_sanitizer':variant=='asan'}
(P/('build-'+variant+'.json')).write_text(json.dumps(r,indent=2)+'\n');print(variant,code,flush=True);raise SystemExit(code!=0)
