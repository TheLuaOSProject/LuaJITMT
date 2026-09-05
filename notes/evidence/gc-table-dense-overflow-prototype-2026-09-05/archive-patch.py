from pathlib import Path
import subprocess,difflib,json,hashlib
p=Path(__file__).parent
base='d680421c4cb50b85437d88255bc89358c5e3a6b1'
files=['src/lj_arena.c','src/lj_arena.h','src/lj_gc2.c','src/lj_gc2.h']
patch='';rows=[]
for f in files:
 old=subprocess.check_output(['git','show',base+':'+f],cwd='/workspaces/lj-lockless').decode()
 new=(p/'strict'/f).read_text()
 patch+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+f,tofile='b/'+f))
 rows.append({'path':f,'sha256':hashlib.sha256(new.encode()).hexdigest()})
(p/'dense-W.patch').write_text(patch)
(p/'source-manifest.json').write_text(json.dumps({'base':base,'files':rows,'patch_sha256':hashlib.sha256(patch.encode()).hexdigest(),'frozen_variants':['strict','normal','asan']},indent=2)+'\n')
print((p/'source-manifest.json').read_text())
