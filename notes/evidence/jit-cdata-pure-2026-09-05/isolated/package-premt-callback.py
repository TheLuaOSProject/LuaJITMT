from pathlib import Path
import shutil,json,hashlib,subprocess
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y');out=r/'callback-failure';out.mkdir(exist_ok=True)
files=[p for p in r.iterdir() if p.is_file() and ('callback' in p.name) and p.suffix in ['.c','.txt','.json','.stdout','.stderr','.ir']]
for p in files:shutil.copy2(p,out/p.name)
for f in ['candidate.patch','candidate-source.json']:shutil.copy2(r/f,out/f)
for f in ['/tmp/run-premt-callback.py','/tmp/run-premt-callback-control.py']:shutil.copy2(f,out/Path(f).name)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources=['src/lj_ccallback.c','src/lj_ccall.c','src/lj_asm_x86.h','src/lj_ffrecord.c','src/lj_snap.c','src/lj_record.c','src/lj_jit.h','src/lj_opt_loop.c','src/lj_opt_mem.c','src/lj_ctype.h','src/lj_tg.h']
manifest={'base':'b4e26564542cb8bfa997a11c6a90e5e0017a2f79','trees':{},'binaries':{}}
for v in ['base-normal','fix-normal','fix-assert']:
 manifest['trees'][v]={f:sha(r/v/f) for f in sources}
 for f in [r/v/'src/luajit',r/v/'src/libluajit.a',r/(v+'-callback-negative'),r/(v+'-callback-control'),r/(v+'-callback-off-control')]:manifest['binaries'][str(f)]=sha(f)
(out/'source-binary-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(out/'review.md').write_text('''# Excluded CALLXS control: confirmed stale callback stack top

This is a baseline stability finding, separate from the optional pure cdata method-load candidate. Exact base is b4e26564542cb8bfa997a11c6a90e5e0017a2f79. Candidate changes only lj_jit.h, lj_opt_loop.c and lj_opt_mem.c; the CALLXS loop is ineligible, and final IR retains its repeated cdata-root/method chain.

`callback-negative.c` warms a real generated CALLXS to C `int32_t invoke(int32_t (*)(int32_t), int32_t)`. Warm execution returns i+1 without invoking the passed live callback. After enabling callbacks, the same function calls the retained callback, which returns the same i+1. For i=1..80, each iteration computes (i+1)+(i+1), so 6640 is the exact result. Generated frame and suspended callback observations use production LJFFINativeFrame snapshots.

Exact base and candidate normal executions return 4 with one native exit; candidate assertion build traps in `lj_assert_bad_for_arg_type`. Explicit interpreter controls return 6640 on all three runtimes. A separate one-change control disables JIT for the callback body and preserves the failure, so callback-entry recording is not required. Existing native assertions were not weakened to obtain a pass.

Hardware watchpoint evidence in `callback-stack-gdb2.stdout` identifies the write: at generated invoke(value=1), L->base/top are stack slots25/29, while the retained native frame base_offset/top_offset describe slots37/40. The for-step at jit_base+7 (stack slot32) starts as double1. `callback_conv_args` writes callback frame word66 to that exact slot at lj_ccallback.c:1100. Post-call SNAP6 leaves the unchanged loop-step slot absent; it therefore remains66 through the strict bad-for-arg-type trap. Snapshot/root references are live and the callback ABI is valid; this is not a malformed allocator/admission setup.

`callback-control-results.json` contains exact compile/run commands and native/interpreter results, and the corresponding `.ir` files record unchanged XSAVE/post-call snapshots. ANSI IR color escapes are preserved. Earlier failure evidence and gdb setup errors are retained, rather than overwritten. `source-binary-manifest.json` names exact archives/executables and relevant source hashes; this package stores text only. No callback runtime fix was made here. Root assigned an independent isolated repair after this diagnosis.
''')
entries={str(p.relative_to(out)):{'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(out.rglob('*')) if p.is_file() and p.name!='artifact-manifest.json'}
(out/'artifact-manifest.json').write_text(json.dumps({'files':entries},indent=2)+'\n')
print(out);print(sha(out/'artifact-manifest.json'));print(len(entries))
