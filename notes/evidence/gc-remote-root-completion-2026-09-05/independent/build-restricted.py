import pathlib,subprocess,json,tarfile,io,time,hashlib,difflib
r=pathlib.Path(__file__).resolve().parent;repo=pathlib.Path('/workspaces/lj-lockless');t=r/'remote-only';t.mkdir()
base='b4e26564542cb8bfa997a11c6a90e5e0017a2f79'
data=subprocess.check_output(['git','archive',base],cwd=repo)
with tarfile.open(fileobj=io.BytesIO(data)) as a:a.extractall(t,filter='data')
for p in ['lj_safepoint.c','lj_safepoint.h']:(t/'src'/p).write_bytes((r/'review-input'/p).read_bytes())
p=t/'src/lj_safepoint.c';old=p.read_text();new=old.replace('int completed = hold &&\n','int completed = hold && native_parked &&\n');assert old!=new and new.count('int completed = hold && native_parked &&')==1;p.write_text(new)
(r/'remote-only-vs-reviewed.patch').write_text(''.join(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/src/lj_safepoint.c',tofile='b/src/lj_safepoint.c')))
flags='-DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT'
cmd=['taskset','-c','0-15','make','-C',str(t/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:', 'XCFLAGS='+flags]
st=time.monotonic();p=subprocess.run(cmd,capture_output=True,text=True);(r/'remote-only-build.stdout').write_text(p.stdout);(r/'remote-only-build.stderr').write_text(p.stderr)
row={'base':base,'command':cmd,'exit':p.returncode,'seconds':time.monotonic()-st,'safepoint_c_sha256':hashlib.sha256((t/'src/lj_safepoint.c').read_bytes()).hexdigest(),'safepoint_h_sha256':hashlib.sha256((t/'src/lj_safepoint.h').read_bytes()).hexdigest()}
if p.returncode==0:row['archive_sha256']=hashlib.sha256((t/'src/libluajit.a').read_bytes()).hexdigest()
(r/'remote-only-build.json').write_text(json.dumps(row,indent=2)+'\n');print(row);assert p.returncode==0
