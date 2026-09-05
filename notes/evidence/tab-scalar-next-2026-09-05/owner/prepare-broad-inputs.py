from pathlib import Path
import hashlib,json,subprocess
p=Path(__file__).resolve().parent
repo=Path('/workspaces/lj-lockless')
out=p/'broad-inputs.json'
assert not out.exists()
paths=subprocess.check_output(['git','ls-tree','-r','--name-only','79345529','--','tests/stock'],cwd=repo,text=True).splitlines()
paths+=['tests/t-tab-scalar-hit.c','tests/t-tab-rooted-reader.c']
rows={}
for name in paths:
    data=subprocess.check_output(['git','show','79345529:'+name],cwd=repo)
    dest=p/'fixtures'/Path(name).relative_to('tests')
    assert not dest.exists()
    dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_bytes(data)
    rows[str(dest)]={'sha256':hashlib.sha256(data).hexdigest(),'git_blob':subprocess.check_output(['git','rev-parse','79345529:'+name],cwd=repo,text=True).strip()}
out.write_text(json.dumps({'base':'79345529','files':rows},indent=2)+'\n')
print('copied',len(rows),'exact793 test inputs')
