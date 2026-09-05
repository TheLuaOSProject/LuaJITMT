#!/usr/bin/env python3
import datetime,hashlib,json,pathlib
P=pathlib.Path(__file__).resolve().parent
excluded={'artifact-manifest.json','manifest-verification.json'}
entries={str(f.relative_to(P)):{'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'bytes':f.stat().st_size} for f in sorted(P.rglob('*')) if f.is_file() and f.name not in excluded}
manifest={'frozen_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'entries':entries,'count':len(entries),'bytes':sum(v['bytes'] for v in entries.values())}
path=P/'artifact-manifest.json';path.write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
sha=hashlib.sha256(path.read_bytes()).hexdigest()
assert all(hashlib.sha256((P/n).read_bytes()).hexdigest()==v['sha256'] and (P/n).stat().st_size==v['bytes'] for n,v in entries.items())
(P/'manifest-verification.json').write_text(json.dumps({'manifest_sha256':sha,'checked_entries':len(entries),'mismatches':[]},indent=2)+'\n')
print('manifest',sha,'entries',len(entries),'bytes',manifest['bytes'])
print('handoff',hashlib.sha256((P/'HANDOFF.md').read_bytes()).hexdigest())
