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
class Snap(gdb.Breakpoint):
 def stop(self):
  try:
   fr=gdb.newest_frame();L=fr.read_var('L');stage=fr.read_var('stage').string();rd=int(fr.read_var('round'))
   d={'kind':'snapshot','stage':stage,'round':rd,'seconds':time.monotonic()-start,'state':state(L)}
   if stage in ('automatic_progress_missing','sole_main_recovered'):d['stack']=gdb.execute('thread apply all bt 20',to_string=True)
   out.write(json.dumps(d,sort_keys=True)+'\n')
  except Exception as e:out.write(json.dumps({'error':str(e),'stack':gdb.execute('bt 10',to_string=True)})+'\n')
  return False
Snap('snapshot')
