from pathlib import Path
import subprocess,tarfile,io,json,time,os,signal,difflib,hashlib,concurrent.futures
r=Path(__file__).resolve().parent;repo=Path('/workspaces/lj-lockless');base='dd2c439179b1e12564710484d8511e4cee617f7f'
archive=subprocess.check_output(['git','archive',base],cwd=repo)
old=subprocess.check_output(['git','show',base+':src/lj_record.c'],cwd=repo,text=True)
block='''    /* The cdata metatable is treated as immutable. */
    if (LJ_HASFFI && tref_iscdata(ix->tab)) {
      mix.tab = TREF_NIL;
      goto immutable_mt;
    }
'''
assert old.count(block)==1
new=old.replace(block,'''    /* Cdata uses the same method guards: its base table's entries can change
    ** without replacing the base root or flushing existing native traces. */
''')
(r/'pre-mt-cdata-method-guards.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/src/lj_record.c',tofile='b/src/lj_record.c')))
for v in ['base-normal','fix-normal','fix-assert']:
 p=r/v;p.mkdir(exist_ok=True)
 with tarfile.open(fileobj=io.BytesIO(archive)) as t:t.extractall(p,filter='data')
 if v!='base-normal':(p/'src/lj_record.c').write_text(new)
helpers='-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
def build(v):
 p=r/v;cmd=['taskset','-c','0-15','make','-C',str(p/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
 if v=='fix-assert':cmd+=['XCFLAGS='+helpers]
 st=time.monotonic();q=subprocess.Popen(cmd,cwd=p,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=120);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 (r/(v+'-build.stdout')).write_text(out);(r/(v+'-build.stderr')).write_text(err)
 print(v,q.returncode,round(time.monotonic()-st,3),flush=True)
 return {'variant':v,'command':cmd,'cwd':str(p),'exit':q.returncode,'status':status,'seconds':time.monotonic()-st,'source_sha256':hashlib.sha256((p/'src/lj_record.c').read_bytes()).hexdigest(),'runtime_sha256':hashlib.sha256((p/'src/luajit').read_bytes()).hexdigest() if (p/'src/luajit').exists() else None}
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:results=list(ex.map(build,['base-normal','fix-normal','fix-assert']))
(r/'guard-build-results.json').write_text(json.dumps({'base':base,'results':results},indent=2)+'\n')
assert all(q['exit']==0 for q in results)
