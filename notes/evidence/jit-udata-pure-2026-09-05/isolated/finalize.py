from pathlib import Path
import subprocess,os,json,re,hashlib,shutil
p=Path(__file__).resolve().parent;rows=[]
for kind in ['clib','file','call']:
 cmd=[str(p/'candidate/src/luajit'),'-jdump=im',str(p/'cost.lua'),kind,'80']
 candidate_raw=(p/('candidate-'+kind+'.ir')).read_text()
 def ir(s):
  out=[]
  for line in s.splitlines():
   line=re.sub(r'\x1b\[[0-9;]*m','',line)
   if re.match(r'^\d{4}\s',line):
    line=re.sub(r'0x[0-9a-fA-F]+','0xADDR',line)
    line=re.sub(r'(lj_ffi_native_trace_enter  \(trace: 0xADDR )\+\d+( NULL\))',r'\1+ADDR\2',line)
    out.append(line)
  return out
 a,b=ir(candidate_raw),ir((p/('guarded-'+kind+'.ir')).read_text());eq=a==b
 rows.append({'command':cmd,'evidence':kind+'.ir','same_normalized_IR_as_guarded':eq,'normalization':'IR lines with four decimal digits followed by whitespace; ANSI removed, hexadecimal addresses and the explicit native-trace-enter decimal pointer operand replaced; no other operation/type/operand/slot normalization','candidate_IR':a,'guarded_IR':b});assert eq,(kind,a,b)
(p/'excluded-cost-ir-results.json').write_text(json.dumps(rows,indent=2)+'\n')
source_identity=json.loads(Path('/tmp/lj-special-udata-method-review-20260905-djl20ksd/runtime-input-identity.json').read_text())
paths=sorted(source_identity['baseline']);manifest={'base':'e34282576c7df0180e8113a4cfba07fd637a36b3','guarded_source_equivalent_to_commit':'9f68fa8d','count':len(paths),'variants':{}}
for variant in ['guarded','candidate','strict','asan']:
 manifest['variants'][variant]={f:hashlib.sha256((p/variant/f).read_bytes()).hexdigest() for f in paths}
assert len(paths)==224
assert manifest['variants']['guarded']==source_identity['candidate_matching_all_three']
assert manifest['variants']['candidate']==manifest['variants']['strict']==manifest['variants']['asan']
changed=[f for f in paths if manifest['variants']['guarded'][f]!=manifest['variants']['candidate'][f]]
assert changed==['src/lj_opt_mem.c'];manifest['only_changes_vs_guarded']=changed
(p/'runtime-input-identity.json').write_text(json.dumps(manifest,indent=2)+'\n')
for variant in ['guarded','candidate']:
 out=p/(variant+'-source');out.mkdir(exist_ok=True)
 for f in ['lj_opt_mem.c','lj_opt_loop.c','lj_jit.h','lj_record.c']:
  shutil.copyfile(p/variant/'src'/f,out/f)
print('candidate-mem',manifest['variants']['candidate']['src/lj_opt_mem.c'])
print('patch',hashlib.sha256((p/'candidate-v2.patch').read_bytes()).hexdigest())
