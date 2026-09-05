from pathlib import Path
import shutil,subprocess,time,json,signal,os,difflib
p=Path(__file__).parent;rows=[];flags='-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT'
def run(name,cmd):
 start=time.monotonic();q=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=100);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 rows.append({'name':name,'command':cmd,'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err});(p/'negative-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,out[-1000:],err[-1500:],flush=True);return q.returncode
for name in ['old-size','old-realloc','body-proof']:
 t=p/('negative-'+name);shutil.copytree(p/'tail/src',t/'src');shutil.copytree(p/'tail/dynasm',t/'dynasm')
 f=t/'src'/('lj_gc2.c' if name=='body-proof' else 'lj_arena.c');old=f.read_text();s=old
 if name=='old-size':
  start=s.index('size_t lj_arena_huge_mapsize(size_t size)');end=s.index('\nvoid *lj_arena_huge_map(',start)
  s=s[:start]+'''size_t lj_arena_huge_mapsize(size_t size)
{
 size_t need=size+sizeof(GCAhdr);
 if(size<=LJ_HUGE_THRESHOLD || need<size || need>~(size_t)LJ_ARENA_MASK)return 0;
 return (need+LJ_ARENA_MASK)&~(size_t)LJ_ARENA_MASK;
}
''' + s[end:]
 elif name=='old-realloc':
  a='lj_arena_huge_mapsize(osize) == lj_arena_huge_mapsize(nsize)'
  b='((osize+sizeof(GCAhdr)+LJ_ARENA_MASK)&~(size_t)LJ_ARENA_MASK) == ((nsize+sizeof(GCAhdr)+LJ_ARENA_MASK)&~(size_t)LJ_ARENA_MASK)'
  assert a in s;s=s.replace(a,b,1)
 else:
  a='''  if (acquired == LJ_ARENA_HUGE_TOKEN_LEASE_DEFERRED) {
    /* DEFER_FREE'''
  b='''  if (acquired == LJ_ARENA_HUGE_TOKEN_LEASE_DEFERRED) {
    /* NEGATIVE: header-only lease is incorrectly used as W/body proof. */
    (void)gc2_table_authority(g, (GCtab *)p);
    /* DEFER_FREE'''
  assert a in s;s=s.replace(a,b,1)
 f.write_text(s);(p/('negative-'+name+'.patch')).write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/src/'+f.name,tofile='b/src/'+f.name)))
 if run('build-'+name,['taskset','-c','0-15','make','-C',str(t/'src'),'-j4','BUILDMODE=static','XCFLAGS='+flags,'CCDEBUG=-g','TARGET_STRIP=:']):continue
 src=p/('traverse-adapter.c' if name=='body-proof' else 't-huge-tail.c');exe=p/('negative-'+name+'-fixture')
 cmd=['taskset','-c','0-15','gcc','-O2','-g','-std=gnu11','-Wall','-Wextra','-Werror','-mcx16']+flags.split()+['-I'+str(t/'src'),'-I'+str(p/'tail/tests'),'-I'+str(p),str(src),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 if name!='body-proof':cmd+=['-Wl,--wrap=mmap','-Wl,--wrap=mmap64','-Wl,--wrap=munmap','-Wl,--wrap=calloc','-Wl,--wrap=free']
 if run('compile-'+name,cmd):continue
 run('test-'+name,['taskset','-c','0-15',str(exe)]+([] if name=='body-proof' else ['payload' if name=='old-size' else 'resize']))
 if name=='body-proof':run('stack-'+name,['taskset','-c','0-15','gdb','-q','-batch','-ex','set pagination off','-ex','run','-ex','thread apply all bt 12','--args',str(exe)])
