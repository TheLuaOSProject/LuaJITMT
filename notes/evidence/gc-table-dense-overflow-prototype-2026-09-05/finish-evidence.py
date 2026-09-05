from pathlib import Path
import json,hashlib,subprocess
p=Path(__file__).parent;base='d680421c4cb50b85437d88255bc89358c5e3a6b1'
def sha(f):return hashlib.sha256(f.read_bytes()).hexdigest()
files=['src/lj_arena.c','src/lj_arena.h','src/lj_gc2.c','src/lj_gc2.h']
unchanged=['src/vm_x64.dasc','src/lj_asm_x86.h','src/lj_tab.c','src/lj_meta.c']
m={'base':base,'working_directory':'/workspaces/lj-lockless','artifact':str(p),'source_variants':{},'fixtures':{},'binaries':{},'builds':{},'runtime_asan_options':'detect_leaks=1:abort_on_error=1','test_cpu_affinity':'0-15','cost_cpu_affinity':'31','system_isolation':'CPU31 reserved; other functional work on0-15 and root workload on30; host/frequency not isolated','tools':{}}
for v in ['strict','normal','asan','base-normal']:
 m['source_variants'][v]={}
 for f in files+unchanged:
  m['source_variants'][v][f]=sha(p/v/f)
  expected=hashlib.sha256(subprocess.check_output(['git','show',base+':'+f],cwd='/workspaces/lj-lockless')).hexdigest()
  if f in unchanged or v=='base-normal':assert m['source_variants'][v][f]==expected
 if v in ['normal','asan']:
  for f in files:assert m['source_variants'][v][f]==m['source_variants']['strict'][f]
 for f in ['src/libluajit.a','src/luajit','src/lj_gc2.o','src/lj_vm.o']:m['binaries'][v+'/'+f]=sha(p/v/f)
for f in p.iterdir():
 if f.is_file() and f.suffix in ['.c','.h','.py','.patch']:m['fixtures'][f.name]=sha(f)
 if f.is_file() and f.name in ['strict-overflow','asan-overflow','strict-tnew','asan-tnew','strict-fnew','asan-fnew','strict-traverse','asan-traverse','strict-t-dense-fnew-consistent','asan-t-dense-fnew-consistent','base-normal-cost','normal-cost']:m['binaries'][f.name]=sha(f)
helpers='-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT'
for v in ['strict','normal','asan','base-normal']:
 cmd=['taskset','-c','0-15','make','-C',str(p/v/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
 env={}
 if v in ['strict','asan']:cmd+=['XCFLAGS='+helpers]
 if v=='asan':
  cmd+=['CC=clang','CCOPT=-O1 -fno-omit-frame-pointer -fsanitize=address','CCOPT_x86=','CCOPT_x64=','TARGET_LDFLAGS=-fsanitize=address','HOST_LDFLAGS=-fsanitize=address'];env={'ASAN_OPTIONS':'detect_leaks=0'}
 m['builds'][v]={'command':cmd,'environment':env,'environment_scope':'build-time code generators only; runtime leak detection enabled' if env else 'default'}
for name,cmd in [('gcc',['gcc','--version']),('clang',['clang','--version']),('kernel',['uname','-a']),('glibc',['getconf','GNU_LIBC_VERSION'])]:m['tools'][name]=subprocess.check_output(cmd,text=True).strip()
for f in ['strict-results.json','asan-results.json','fnew-consistent-results.json','negative-results.json','stock-results.json','cost-results.json','cost-summary.json','layout.json','base-stock-results.json','audit.md','dense-W.patch','dense-W-candidate.patch','candidate-source-manifest.json']:
 m.setdefault('evidence',{})[f]=sha(p/f)
m['qualifications']=[
 'strict/asan fnew-existing rows use settled inherited setup and are diagnostic only: unchanged base forged allocf_arena while arena allocator remained selected.',
 'Final valid full FNEW is t-dense-fnew-consistent.c, with mt_entering eligibility fallback and no unfinished CONSTRUCT/LINKING assertion; fnew-consistent-results.json.',
 'Development implement/adapt scripts preserve history; reproduce from dense-W.patch plus final hashed fixtures, not by rerunning intermediate edit scripts.',
 'dense-W-candidate.patch differs from validated/measured source only by requested small-only accessor precondition comment.',
 'Initial stock wrong-cwd and initial barrier counter setup, compilation diagnostics, and original full FNEW fixture failures preserved.',
 'Runtime artifacts and source are isolated on d680, excluding later final arena/scalar/direct-hit changes.'
]
(p/'final-validation.json').write_text(json.dumps(m,indent=2)+'\n')
print('manifest',sha(p/'final-validation.json'))
