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

import struct,collections
inf=gdb.selected_inferior()
arenat=gdb.lookup_type('GCArena').pointer()
def words(v):
 return struct.unpack('<'+'Q'*(v.type.sizeof//8),bytes(inf.read_memory(int(v.address),v.type.sizeof)))
def object_info(addr):
 aaddr=addr&~65535;cell=(addr-aaddr)//16;a=gdb.Value(aaddr).cast(arenat).dereference()
 planes={n:words(a[n]) for n in ('block','ready','mark','late','root','lifetime','recovery','sweep')}
 d={'address':addr,'arena':aaddr,'cell':cell,'arena_header':scalars(a['hdr'])}
 for n in ('block','ready','mark','late'):d[n]=(planes[n][cell//64]>>(cell%64))&1
 for n in ('root','recovery','sweep'):d[n]=(planes[n][cell//32]>>(2*(cell%32)))&3
 d['lifetime']=(planes['lifetime'][cell//16]>>(4*(cell%16)))&15
 d['dtor']=sum(((words(a['dtor'][i])[cell//64]>>(cell%64))&1)<<i for i in range(4))
 if d['block'] and d['ready']:
  t=gdb.Value(addr).cast(gdb.lookup_type('GCtab').pointer()).dereference()
  d['gct']=int(t['gct']);d['marked']=int(t['marked']);d['nextgc']=int(t['nextgc']['gcptr64'])
  if d['gct']==11:
   d['table']={n:simple(t[n]) for n in ('asize','hmask','gc2_rescan_state','colo','nomm')}
   d['table']['array']=int(t['array']['ptr64']);d['table']['array_prefix_raw']=bytes(inf.read_memory(int(t['array']['ptr64']),min(int(t['asize']),4)*8)).hex() if int(t['array']['ptr64']) else ''
 return d
def retained(L):
 g=glob(L).dereference();tg=g['main_tg'].dereference();q=g['gc2'];d={'ssb':{},'arenas':[]}
 samples=set()
 for name,nodep in [('drain',q['ssb_drain']),('head',q['ssb_head'])]:
  if not int(nodep):d['ssb'][name]=None;continue
  n=nodep.dereference();count=int(n['n']);vals=list(words(n['slot']))[:count]
  d['ssb'][name]={'address':int(nodep),'next':int(n['next']),'owner':int(n['owner']),'count':count,'slots':vals,'unique_count':len(set(vals))}
  samples.update(vals[:4]+vals[-4:])
 count=int(tg['ssb_next']-tg['ssb_base']);vals=list(struct.unpack('<'+'Q'*count,bytes(inf.read_memory(int(tg['ssb_base']),count*8))))
 d['ssb']['private']={'count':count,'slots':vals,'unique_count':len(set(vals))};samples.update(vals[:4]+vals[-4:])
 aseen=set();reccount=collections.Counter();lifecount=collections.Counter();rootcount=collections.Counter();readycount=collections.Counter();blockcount=collections.Counter();typecount=collections.Counter();cap=False
 for lane in ('owned','needsweep','quarantine','reclaimed'):
  arr=tg['alloc'][lane]
  for k in range(arr.type.range()[1]+1):
   ap=arr[k];seen=set()
   while int(ap) and int(ap) not in seen:
    addr=int(ap);seen.add(addr)
    if len(aseen)>=2048:cap=True;break
    a=ap.dereference();nxt=a['hdr']['next']
    if addr not in aseen:
     aseen.add(addr);rec=words(a['recovery']);life=words(a['lifetime']);root=words(a['root']);ready=words(a['ready']);block=words(a['block']);raw=bytes(inf.read_memory(addr,65536));cells=[]
     for wi,w in enumerate(rec):
      if not w:continue
      for off in range(32):
       state=(w>>(off*2))&3
       if state:
        c=wi*32+off;cells.append(c);reccount[state]+=1;lifecount[(life[c//16]>>(4*(c%16)))&15]+=1;rootcount[(root[c//32]>>(2*(c%32)))&3]+=1;readycount[(ready[c//64]>>(c%64))&1]+=1;blockcount[(block[c//64]>>(c%64))&1]+=1
        if (ready[c//64]>>(c%64))&1 and (block[c//64]>>(c%64))&1:typecount[raw[c*16+9]]+=1
     d['arenas'].append({'address':addr,'lane':lane,'kind':k,'header':scalars(a['hdr']),'recovery_cells':cells})
     if cells:samples.add(addr+cells[0]*16);samples.add(addr+cells[-1]*16)
    ap=nxt
 d['arena_cap_reached']=cap;d['recovery_state_counts']=dict(reccount);d['recovery_lifetime_counts']=dict(lifecount);d['recovery_root_counts']=dict(rootcount);d['recovery_count_from_planes']=sum(reccount.values());d['recovery_ready_counts']=dict(readycount);d['recovery_block_counts']=dict(blockcount);d['ready_recovery_gct_counts']=dict(typecount);d['published_recovery_items']=int(q['recovery_items'])
 # Keep payload interpretation bounded to exact queue frontiers and first/last recovery arenas.
 samples=sorted(x for x in samples if x)
 selected=samples[:8]+samples[-8:]
 for lane in ('drain','private'):
  if d['ssb'][lane]:selected+=d['ssb'][lane]['slots'][:2]+d['ssb'][lane]['slots'][-2:]
 d['sample_objects']=[object_info(x) for x in sorted(set(selected)) if x]
 pend=int(tg['gcroot_pending']);d['pending_root_prefix']=[]
 for i in range(8):
  if not pend:break
  oi=object_info(pend);d['pending_root_prefix'].append(oi);pend=oi.get('nextgc',0)&~3
 return d

class Snap(gdb.Breakpoint):
 def stop(self):
  try:
   fr=gdb.newest_frame();L=fr.read_var('L');stage=fr.read_var('stage').string();rd=int(fr.read_var('round'))
   d={'kind':'snapshot','stage':stage,'round':rd,'seconds':time.monotonic()-start,'state':state(L)}
   if stage in ('automatic_progress_missing','sole_main_recovered'):d['stack']=gdb.execute('thread apply all bt 20',to_string=True)
   if stage=='automatic_progress_missing':d['retained']=retained(L)
   out.write(json.dumps(d,sort_keys=True)+'\n')
  except Exception as e:out.write(json.dumps({'error':str(e),'stack':gdb.execute('bt 10',to_string=True)})+'\n')
  return False
Snap('snapshot')
