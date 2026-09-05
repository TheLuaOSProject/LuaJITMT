from pathlib import Path
import subprocess,difflib,json,hashlib
p=Path(__file__).parent;base='d680421c4cb50b85437d88255bc89358c5e3a6b1'
files=['src/lj_arena.c','src/lj_arena.h','src/lj_gc2.c','src/lj_gc2.h'];patch='';rows=[]
for name in files:
 s=(p/'strict'/name).read_text();new=s
 if name=='src/lj_arena.h':
  old='''static LJ_AINLINE LJGC2TabStampArena *
lj_arena_gc2_tabstamp_acq(const GCArena *a)'''
  assert old in s
  new=s.replace(old,'''/* Small-mapping-only accessor. The caller retains the exact non-Huge
** mapping; immutable mapping kind selects this union arm. Plain small maps
** return NULL. Huge body-proof users must use lj_arena_gc2_wide_acq after
** their separate readable-body admission; header-only token users stay W-blind. */
static LJ_AINLINE LJGC2TabStampArena *
lj_arena_gc2_tabstamp_acq(const GCArena *a)''',1)
  (p/'accessor-comment.patch').write_text(''.join(difflib.unified_diff(s.splitlines(True),new.splitlines(True),fromfile='a/'+name,tofile='b/'+name)))
 f=p/'candidate'/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(new)
 old=subprocess.check_output(['git','show',base+':'+name],cwd='/workspaces/lj-lockless').decode()
 patch+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+name,tofile='b/'+name))
 rows.append({'path':name,'sha256':hashlib.sha256(new.encode()).hexdigest()})
(p/'dense-W-candidate.patch').write_text(patch)
(p/'candidate-source-manifest.json').write_text(json.dumps({'base':base,'files':rows,'patch_sha256':hashlib.sha256(patch.encode()).hexdigest(),'difference_from_tested_and_measured_patch':'One small-only accessor precondition comment in src/lj_arena.h; no code changes.'},indent=2)+'\n')
