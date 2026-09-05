from pathlib import Path
import hashlib,json,os,re,subprocess
p=Path(__file__).resolve().parent;old=Path('/tmp/lj-special-udata-pure-20260905-u7z61i10');tree=p/'candidate';env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';;';rows=[]
def clean(s):return re.sub(r'\x1b\[[0-9;]*m','',s)
def machine_loop(s):
 s=clean(s);part=s.split('->LOOP:\n',1)[1].split('---- TRACE',1)[0];out=[];start=None
 for line in part.splitlines():
  m=re.match(r'^([0-9a-f]{8,16})\s+(.*)$',line)
  if m:
   address=int(m.group(1),16)
   if start is None:start=address
   out.append([address-start,re.sub(r'0x[0-9a-fA-F]{8,16}','0xADDR',m.group(2))])
 return out,start%4096
for kind in ['clib','file','buffer','plain']:
 fixture=p/'direct-cost.lua';cmd=['taskset','-c','24-27',str(tree/'src/luajit'),'-jdump=im',str(fixture),kind,'80']
 r=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=20)
 (p/(kind+'.ir')).write_text(r.stdout);(p/(kind+'.stderr')).write_text(r.stderr);assert r.returncode==0,r.stderr
 previous=(old/('candidate-direct-'+kind+'.ir')).read_text();a,aa=machine_loop(previous);b,bb=machine_loop(r.stdout)
 row={'kind':kind,'command':cmd,'cwd':str(tree),'exit':r.returncode,'environment':{'LUA_PATH':env['LUA_PATH']},'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),'binary_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest(),'prior_ir_sha256':hashlib.sha256(previous.encode()).hexdigest(),'previous_shape':[x for x in clean(previous).splitlines() if x.startswith('shape')],'current_shape':[x for x in clean(r.stdout).splitlines() if x.startswith('shape')],'same_hot_loop_assembly':a==b,'same_loop_page_offset':aa==bb,'previous_loop_page_offset':aa,'current_loop_page_offset':bb,'previous_loop_assembly':a,'current_loop_assembly':b}
 rows.append(row);print(kind,row['previous_shape'],row['current_shape'],'same hot assembly',a==b,'offset',aa,bb)
(p/'ir-check-results.json').write_text(json.dumps(rows,indent=2)+'\n')
