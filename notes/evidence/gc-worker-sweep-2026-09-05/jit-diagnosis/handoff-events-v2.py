import gdb,json,time
out=open(OUTFILE,'w',buffering=1)
start=time.monotonic()
ptr=gdb.lookup_type('global_State').pointer()
def simple(v):
 t=v.type.strip_typedefs()
 if t.code in (gdb.TYPE_CODE_INT,gdb.TYPE_CODE_ENUM,gdb.TYPE_CODE_BOOL,gdb.TYPE_CODE_PTR):return int(v)
 return str(v)
def scalars(v):
 r={}
 for f in v.type.strip_typedefs().fields():
  if f.name:
   x=v[f.name]
   if x.type.strip_typedefs().code in (gdb.TYPE_CODE_INT,gdb.TYPE_CODE_ENUM,gdb.TYPE_CODE_BOOL,gdb.TYPE_CODE_PTR):r[f.name]=simple(x)
 return r
def glob(L):return L.dereference()['glref']['ptr64'].cast(ptr)
def state(L):
 gp=glob(L);g=gp.dereference();tg=g['main_tg'].dereference();gc=g['gc'];alloc=tg['alloc']
 d={'g':int(gp),'gc2':scalars(g['gc2']),'global':scalars(g),'gc':scalars(gc),'tg':scalars(tg),'alloc':scalars(alloc),'root':int(gc['root']['gcptr64']),'gc_gray':int(gc['gray']['gcptr64']),'gc_grayagain':int(gc['grayagain']['gcptr64']),'ssb_private_count':int(tg['ssb_next']-tg['ssb_base']),'alloc_lists':{}}
 for n in ('owned','needsweep','quarantine','reclaimed'):
  a=alloc[n];lo,hi=a.type.range();d['alloc_lists'][n]=[int(a[i]) for i in range(lo,hi+1)]
 d['alloc_bumps']=[{'a':int(alloc['bump'][i]['a']),'cell':int(alloc['bump'][i]['cell']),'end':int(alloc['bump'][i]['end'])} for i in range(alloc['bump'].type.range()[1]+1)]
 cur=g['gc2']['sweep_root_cursor']
 if int(cur):d['sweep_cursor_target']=int(cur.dereference()['gcptr64'])
 return d
active=False
events=[]
counts={}
class Snap(gdb.Breakpoint):
 def stop(self):
  try:
   global active
   fr=gdb.newest_frame();L=fr.read_var('L');stage=fr.read_var('stage').string();rd=int(fr.read_var('round'))
   if stage=='after_churn' and rd==4:
    active=True
    for bp in events: bp.enabled=True
   if stage in ('automatic_progress_missing','sole_main_recovered'): active=False
   d={'kind':'snapshot','stage':stage,'round':rd,'seconds':time.monotonic()-start,'state':state(L)}
   if stage in ('automatic_progress_missing','sole_main_recovered'):d['stack']=gdb.execute('thread apply all bt 20',to_string=True)
   out.write(json.dumps(d,sort_keys=True)+'\n')
  except Exception as e:out.write(json.dumps({'error':str(e),'stack':gdb.execute('bt 10',to_string=True)})+'\n')
  return False
Snap('snapshot')

def quick(gp):
 g=gp.dereference();q=g['gc2'];tg=g['main_tg'].dereference()
 keys=('phase','cycle','sweep_to_idle','jit_phase_gate','jit_sweep_displaced','jit_sweep_yield_until_ns','jit_hard_checks','sweep_bridge_ready','sweep_root_scanned','sweep_root_done','sweep_owner_runs','sweep_owner_arenas','ssb_items_published','ssb_items_drained','recovery_items','recovery_drained','grey_top','grey_bottom','worker_active','worker_busy_retries','deferred_epoch','smr_readers','smr_reclaiming','table_rescan_pending','alloc_since_trigger','hard_check_bytes')
 d={k:int(q[k]) for k in keys}
 d.update(jit_base=int(tg['jit_base']),poll=int(tg['poll']),private_ssb=int(tg['ssb_next']-tg['ssb_base']),threshold=int(g['gc']['threshold']),total=int(g['gc']['total']))
 return d
class Event(gdb.Breakpoint):
 def __init__(self,name,isL=False):
  super().__init__('*'+name);self.n=name;self.isL=isL;self.enabled=False
 def stop(self):
  if not active:return False
  try:
   arg=gdb.parse_and_eval('$rdi');gp=glob(arg.cast(gdb.lookup_type('lua_State').pointer())) if self.isL else arg.cast(ptr)
   q=quick(gp)
   if q['cycle']<14:return False
   counts[self.n]=counts.get(self.n,0)+1
   d={'kind':'event','name':self.n,'ordinal':counts[self.n],'seconds':time.monotonic()-start,'state':q,'rsi':int(gdb.parse_and_eval('$rsi')),'rdx':int(gdb.parse_and_eval('$rdx'))}
   if counts[self.n]<=5 or counts[self.n] in (10,20,30,40,50,100,200):d['stack']=gdb.execute('bt 12',to_string=True)
   out.write(json.dumps(d,sort_keys=True)+'\n')
   if sum(counts.values())>=3000:
    for bp in events:bp.enabled=False
    out.write(json.dumps({'kind':'diagnostic_event_cap','count':3000})+'\n')
  except Exception as e:out.write(json.dumps({'error':str(e),'event':self.n})+'\n')
  return False
for name,isL in [('gc2_step_auto',True),('lj_gc2_jit_sweep_request_exit',False),('gc2_jit_phase_gate_open_sweep',False)]:events.append(Event(name,isL))
