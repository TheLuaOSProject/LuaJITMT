from pathlib import Path
import hashlib,json,subprocess,tarfile,re,datetime
P=Path(__file__).resolve().parent;T=P/'tree';sha=lambda b:hashlib.sha256(b).hexdigest()
m=json.loads((P/'source-test-manifest.json').read_text());snapshot=json.loads((P/'strict-snapshot.json').read_text())
old_comment='/* Test-only namespace compression. The caller owns private storage, holds\n** exact allocation authority, or has stopped every actor that can mutate W.\n** A sampled address or stamp alone is not permission to read a body. */'
new_comment='/* Test-only namespace compression. Retain storage authority and exclude\n** concurrent proof updates through private ownership or a paused test schedule.\n** A sampled address or stamp alone is not permission to read a body. */'
header='tests/lib/gc2_wide_fixture_helpers.h';new=(T/header).read_text();old=new.replace(new_comment,old_comment)
assert sha(old.encode())==snapshot[header]
assert re.sub(r'/\*.*?\*/','',old,flags=re.S)==re.sub(r'/\*.*?\*/','',new,flags=re.S)
for f,h in snapshot.items():
 if f!=header:assert sha((T/f).read_bytes())==h,(f,h)
with tarfile.open(P/'base.tar') as tf:
 base={f.name:tf.extractfile(f).read() for f in tf.getmembers() if f.isfile() and f.name.startswith('src/')}
expected={f['path']:f['sha256'] for f in m['files'] if f['group']=='source'}
inventory=[]
for f,b in sorted(base.items()):
 for tree in ['tree','canonical']:
  actual=sha((P/tree/f).read_bytes());assert actual==expected.get(f,sha(b)),(tree,f)
 inventory.append({'path':f,'sha256':sha((T/f).read_bytes())})
for file in ['strict-results.json','canonical-results.json']:
 rows=json.loads((P/file).read_text());assert all(r['exit']==0 and r['status']=='complete' for r in rows)
out={'base':m['base'],'frozen_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'source_and_tests':m,'production_inventory':inventory,'production_files_checked_per_variant':len(inventory),'variants_checked':['tree','canonical'],'strict_results':'strict-results.json','canonical_results':'canonical-results.json','failed_or_timed_out_cases':0,'strict_build_recipe':['taskset','-c','0-15','make','-C',str(T/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:','XCFLAGS=-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT'],'post_validation_change':{'path':header,'old_sha256':snapshot[header],'final_sha256':sha(new.encode()),'old_comment':old_comment,'new_comment':new_comment,'executable_text_unchanged_after_comment_removal':True},'study_boundary':'Prior immutable 28de study has exact same four candidate source hashes; ASan, stock and performance stay qualified to its complete source base. No new timing or ASan run here.','artifacts':[]}
for f in sorted(P.iterdir()):
 if f.is_file() and f.name not in ['final-validation.json','final-validation.sha256']:
  out['artifacts'].append({'path':f.name,'bytes':f.stat().st_size,'sha256':sha(f.read_bytes())})
(P/'final-validation.json').write_text(json.dumps(out,indent=2)+'\n');(P/'final-validation.sha256').write_text(sha((P/'final-validation.json').read_bytes())+'  final-validation.json\n')
print(len(inventory),'production files per variant verified; all tests pass.')
print((P/'final-validation.sha256').read_text(),end='')
