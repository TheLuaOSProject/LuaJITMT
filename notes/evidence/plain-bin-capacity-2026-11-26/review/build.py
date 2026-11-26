from pathlib import Path
import hashlib,json,os,resource,shutil,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
root=Path('/workspaces/lj-lockless')
mode=sys.argv[1]
out=p/(sys.argv[2] if len(sys.argv)>2 else mode)
out.mkdir(exist_ok=False)
files=subprocess.check_output(['git','ls-files','--','src','dynasm','Makefile','tests','tools','bench','doc'],cwd=root,text=True).splitlines()
inputs={}
for rel in files:
 source=root/rel
 if not source.is_file(): continue
 dest=out/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
 inputs[rel]=hashlib.sha256(source.read_bytes()).hexdigest()
for rel in ['tests/t-buffer-progress.c','tests/t-buffer-concurrent-public.lua','tests/t-gc2-small-lease-span.c','tests/t-arena-plain-late.c','tests/t-arena-plain-progress.c','tests/t-arena-plain-bin-progress.c','tests/t-arena-plain-shrink.c','tests/t-arena-bin-capacity.c']:
 source=root/rel
 if source.exists():
  shutil.copy2(source,out/rel);inputs[rel]=hashlib.sha256(source.read_bytes()).hexdigest()
cc='gcc' if mode in ['gcc','tsan','noffi','nobuf','nojit'] else 'clang'
xflags=[] if mode=='gcc' else ['-DLUA_USE_ASSERT']
if mode in ['assert','asan','tsan']: xflags+=['-DLJ_BUF_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS']
cflags=[];ldflags=[]
if mode=='assert':cflags=['-ftrivial-auto-var-init=pattern']
if mode=='asan':cflags=['-fsanitize=address','-fno-omit-frame-pointer'];ldflags=['-fsanitize=address']
if mode=='tsan':cflags=['-fsanitize=thread','-fno-omit-frame-pointer','-fno-pie'];ldflags=['-fsanitize=thread','-no-pie'];xflags+=['-Wno-error=tsan']
if mode in ['noffi','nobuf','nojit']:xflags+=['-DLUAJIT_DISABLE_'+{'noffi':'FFI','nobuf':'BUFFER','nojit':'JIT'}[mode]]
cmd=['make','-C','src','-j5','BUILDMODE=static','CC='+cc,'XCFLAGS='+' '.join(xflags),'TARGET_CFLAGS='+' '.join(cflags),'TARGET_LDFLAGS='+' '.join(ldflags),'TARGET_STRIP=:']
meta=dict(mode=mode,cc=cc,xcflags=xflags,cflags=cflags,ldflags=ldflags,command=cmd,source_inputs=inputs)
(out/'build.json').write_text(json.dumps(meta,indent=2)+'\n')
t=time.monotonic()
with (out/'build.log').open('wb') as f:code=subprocess.run(cmd,cwd=out,stdout=f,stderr=subprocess.STDOUT).returncode
meta.update(exit=code,seconds=time.monotonic()-t)
if code==0:
 meta['outputs']={name:hashlib.sha256((out/'src'/name).read_bytes()).hexdigest() for name in ['luajit','libluajit.a']}
(out/'build.json').write_text(json.dumps(meta,indent=2)+'\n')
print(mode,code,round(meta['seconds'],3),flush=True)
sys.exit(code)
