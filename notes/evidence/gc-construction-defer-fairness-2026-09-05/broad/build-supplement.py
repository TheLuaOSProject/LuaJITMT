from pathlib import Path
import json,hashlib,subprocess,time,sys
P=Path(__file__).resolve().parent
flags='-DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS'
for v in sys.argv[1:]:
 f=flags+(' -DLJ_ARENA_TEST_HELPERS' if v.startswith('arena') else '')
 argv=['make','-C','src','-j4','BUILDMODE=static','XCFLAGS='+f,'TARGET_STRIP=:']
 if v.endswith('asan'):argv+=['CC=clang','HOST_CC=clang','CCOPT=-O1','CCDEBUG=-g','TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer','TARGET_LDFLAGS=-fsanitize=address']
 start=time.monotonic();timed=False
 with (P/(v+'-build.stdout')).open('wb') as out,(P/(v+'-build.stderr')).open('wb') as err:
  try:r=subprocess.run(argv,cwd=P/v,stdout=out,stderr=err,timeout=180);code=r.returncode
  except subprocess.TimeoutExpired:timed=True;code=124
 paths=[P/v/'src'/n for n in ['libluajit.a','luajit','host/minilua','host/buildvm']]
 row=dict(argv=argv,cwd=str(P/v),timeout_seconds=180,exit_code=code,timed_out=timed,seconds=time.monotonic()-start,stdout=v+'-build.stdout',stderr=v+'-build.stderr',binaries={str(q):dict(sha256=hashlib.sha256(q.read_bytes()).hexdigest(),bytes=q.stat().st_size) for q in paths if q.exists()})
 (P/(v+'-build.json')).write_text(json.dumps(row,indent=2)+'\n');print(json.dumps(dict(variant=v,exit_code=code,seconds=row['seconds'])),flush=True)
 if code:raise SystemExit(code)
