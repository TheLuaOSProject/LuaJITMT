from pathlib import Path
import subprocess,time,json,hashlib,os
p=Path(__file__).parent
for kind in ['normal','assert','asan','negative']:
 cmd=['taskset','-c','0-15','make','-C',str(p/kind/'src'),'-j8','BUILDMODE=static']
 env=os.environ.copy()
 if kind!='normal':cmd+=['XCFLAGS=-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT']
 if kind=='asan':
  cmd+=['CC=clang','CCOPT=-O1 -g -fno-omit-frame-pointer -fsanitize=address','CCOPT_x86=','CCOPT_x64=','TARGET_LDFLAGS=-fsanitize=address','HOST_LDFLAGS=-fsanitize=address'];env['ASAN_OPTIONS']='detect_leaks=0'
 start=time.monotonic()
 with (p/('build-'+kind+'.log')).open('w') as out:r=subprocess.run(cmd,env=env,stdout=out,stderr=subprocess.STDOUT,timeout=120)
 rec={'kind':kind,'command':cmd,'exit':r.returncode,'seconds':time.monotonic()-start,'build_ASAN_OPTIONS':env.get('ASAN_OPTIONS'),'binary_sha256':hashlib.sha256((p/kind/'src/luajit').read_bytes()).hexdigest() if r.returncode==0 else None}
 (p/('build-'+kind+'.json')).write_text(json.dumps(rec,indent=2)+'\n');print(json.dumps(rec),flush=True);assert r.returncode==0
