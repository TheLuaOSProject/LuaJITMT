"""All-stop, read-only debugger observations of the unchanged fixed-bound loop.

No inferior calls, register writes, gate changes, or collectors are injected.
The debugger stops perturb scheduling; these runs are diagnostic, not the
uninstrumented completion oracle. Raw body reads are debugger observations,
not a proposal that a runtime reader may ignore lifetime admission.
"""
import collections
import hashlib
import struct
import gdb
import json
import os
from pathlib import Path

out = Path(os.environ['SWEEP_GDB_OUT'])
out.mkdir(exist_ok=False)
seen = set()
pending = None
tracked_objects = set()
types = {}

def typ(name):
    if name not in types:
        types[name] = gdb.lookup_type(name)
    return types[name]

def ptr(addr, name):
    return gdb.Value(int(addr)).cast(typ(name).pointer())

def integer(v):
    return int(v)

def plain(v, depth=0):
    t = v.type.strip_typedefs()
    if t.code == gdb.TYPE_CODE_PTR:
        return hex(int(v))
    if t.code in (gdb.TYPE_CODE_INT, gdb.TYPE_CODE_ENUM, gdb.TYPE_CODE_BOOL):
        return int(v)
    if t.code == gdb.TYPE_CODE_ARRAY:
        lo, hi = t.range()
        if hi-lo > 16:
            return {'array_count': hi-lo+1, 'address': hex(int(v.address))}
        return [plain(v[i], depth+1) for i in range(lo,hi+1)]
    if t.code in (gdb.TYPE_CODE_STRUCT,gdb.TYPE_CODE_UNION) and depth < 5:
        result = {}
        for f in t.fields():
            if f.name:
                try:
                    result[f.name] = plain(v[f.name],depth+1)
                except Exception as e:
                    result[f.name] = {'error':str(e)}
        return result
    return str(v)

def scalar_struct(v):
    return {f.name:plain(v[f.name]) for f in v.type.strip_typedefs().fields()
            if f.name and v[f.name].type.strip_typedefs().code != gdb.TYPE_CODE_ARRAY}

def ref(v):
    return int(v['gcptr64'])

def refs(base,n):
    return [hex(ref(base[i])) for i in range(min(n,1024))]

def arena_header(address):
    a=ptr(address,'GCArena').dereference()
    result=scalar_struct(a['hdr'])
    result['address']=hex(address)
    return result

def arena_list(start):
    rows=[];done=set();address=int(start)
    while address and address not in done and len(rows)<2048:
        done.add(address)
        h=arena_header(address);rows.append(h)
        address=int(ptr(address,'GCArena')['hdr']['next'])
    return {'nodes':rows,'end':hex(address),'bounded':len(rows)==2048}

def ssb_node(start):
    rows=[];done=set();address=int(start)
    while address and address not in done and len(rows)<16:
        done.add(address)
        n=ptr(address,'GC2SSBNode').dereference()
        count=int(n['n'])
        rows.append({'address':hex(address),'next':hex(int(n['next'])),
                     'owner':hex(int(n['owner'])),'n':count,'pad':int(n['pad']),
                     'slots':refs(n['slot'],count)})
        address=int(n['next'])
    return {'nodes':rows,'end':hex(address),'bounded':len(rows)==16}

def lua_state(address):
    L=ptr(address,'lua_State').dereference()
    result=scalar_struct(L)
    stack=int(L['stack']['ptr64']);top=int(L['top'])
    n=(top-stack)//8 if top>=stack else 0
    result['active_stack_count']=n
    result['active_stack_words']=[hex(int(ptr(stack,'TValue')[i]['u64']))
                                  for i in range(min(n,256))]
    return result

def tg_state(address):
    t=ptr(address,'TGState').dereference()
    names='poll profile_request mark_active gl cur_L jit_base vmstate in_native strtab_active_hdr strtab_active_depth strtab_active_epoch strq_active_hdr strq_active_depth strq_active_epoch tab_read_depth tab_read_epoch gc_assist hookmask_th tg_flags fini_state reqmask hs_epoch_ack ssb_active ssb_free ssb_next ssb_base ssb_end ssb_refs gcroot_pending gcroot_pending_after_main root_anchor_top thread_L thread_ud tid actor_id next_tg local_total stack_dirty_epoch root_desc registry_key ffi_native_seq ffi_native_depth'.split()
    result={'address':hex(address)}
    result.update({n:plain(t[n]) for n in names})
    base=int(t['ssb_base']);end=int(t['ssb_next'])
    n=(end-base)//8 if base and end>=base and end-base<=8192 else -1
    result['private_ssb_count']=n
    result['private_ssb_slots']=refs(t['ssb_base'],n) if n>=0 else []
    result['alloc']=scalar_struct(t['alloc'])
    result['arena_lists']={f'{name}{lane}':arena_list(t['alloc'][name][lane])
                            for name in ['owned','needsweep','quarantine','reclaimed']
                            for lane in range(2)}
    result['states']={}
    for field in ['thread_L','cur_L']:
        if int(t[field]) and hex(int(t[field])) not in result['states']:
            result['states'][hex(int(t[field]))]=lua_state(int(t[field]))
    return result

def object_state(address):
    result={'address':hex(address)}
    try:
        arena=address & ~65535
        a=ptr(arena,'GCArena').dereference()
        cell=(address-arena)//16
        result['arena']=arena_header(arena)
        result['cell']=cell
        bits={}
        for field,width in [('block',1),('mark',1),('sweep',2),('root',2),
                             ('lifetime',4),('recovery',2),('ready',1),('late',1),('cdata',1)]:
            bits[field]=(int(a[field][cell//(64//width)]) >> ((cell%(64//width))*width)) & ((1<<width)-1)
        bits['dtor']=sum(((int(a['dtor'][p][cell//64])>>(cell%64))&1)<<p for p in range(4))
        result['bits']=bits
        if bits['block'] and bits['ready']:
            h=ptr(address,'GChead').dereference()
            result['header']=plain(h)
            gct=int(h['gct'])
            if gct==11: # LJ_TTAB is complemented in the GC header.
                t=ptr(address,'GCtab').dereference()
                result['table']=plain(t)
                result['table'].update({n:plain(t[n]) for n in ['acap','struct_owner','struct_control','weak_record']})
                array=int(t['array']['ptr64']);n=int(t['asize'])
                result['array_words']=[hex(int(ptr(array,'TValue')[i]['u64']))
                                       for i in range(min(n,65))] if array else []
                stamp=a['hdr']['gc2_tabstamp']
                if int(stamp):
                    result['tabstamp_type']=str(stamp.type)
                    result['tabstamp']=plain(stamp.dereference()['cell'][cell])
                    result['tabstamp_wide']=plain(stamp.dereference()['wide'][cell])
            elif gct==8:
                fn=ptr(address,'GCfuncL').dereference()
                if int(fn['ffid']):
                    fn=ptr(address,'GCfuncC').dereference()
                    result['function_c_symbol']=gdb.execute('info symbol '+hex(int(fn['f'])),to_string=True)
                result['function']=plain(fn)
    except Exception as e:
        result['error']=str(e)
    return result

def lua_stack_functions(tgs):
    result=[]
    for tg in tgs:
        if tg['tid']!=1:continue
        for state in tg['states'].values():
            for word in state['active_stack_words']:
                raw=int(word,16)
                if raw>>47 != ((1<<17)-9):continue
                address=raw&((1<<47)-1)
                try:
                    fn=ptr(address,'GCfuncL').dereference()
                    if int(fn['gct'])!=8 or int(fn['ffid'])!=0:continue
                    pc=int(fn['pc']['ptr64'])
                    pt=ptr(pc-typ('GCproto').sizeof,'GCproto').dereference()
                    st=ptr(ref(pt['chunkname']),'GCstr').dereference()
                    chunk=bytes(gdb.selected_inferior().read_memory(int(st.address)+typ('GCstr').sizeof,min(int(st['len']),256))).decode('utf-8','replace')
                    uvs=fn['uvptr'].address.cast(typ('GCRef').pointer())
                    row={'function':hex(address),'chunk':chunk,'firstline':int(pt['firstline']),'upvalues':[]}
                    for i in range(min(int(fn['nupvalues']),16)):
                        uv=ptr(ref(uvs[i]),'GCupval').dereference()
                        tv=ptr(int(uv['v']['ptr64']),'TValue').dereference()
                        raw=int(tv['u64'])
                        row['upvalues'].append({'uv':hex(int(uv.address)),'closed':int(uv['closed']),'value':hex(raw),'object_pointer':hex(raw&((1<<47)-1))})
                    result.append(row)
                except Exception as e:result.append({'function':hex(address),'error':str(e)})
    return result

def root_chains(g,tgs):
    # Read-only all-stop decode of exact intrusive ownership identities. Full
    # per-arena chunks avoid hundreds of thousands of debugger value reads.
    cache={};offsets={f.name:f.bitpos//8 for f in typ('GCArena').fields() if f.name}
    def memory(address,n):
        a=address & ~65535
        if a not in cache:
            cache[a]=bytes(gdb.selected_inferior().read_memory(a,65536))
        offset=address-a
        if offset+n<=65536:return cache[a][offset:offset+n]
        return bytes(gdb.selected_inferior().read_memory(address,n))
    def planes(address):
        a=address & ~65535;cell=(address-a)//16
        data={}
        for f,width in [('block',1),('mark',1),('root',2),('lifetime',4),('ready',1),('sweep',2)]:
            word=struct.unpack('<Q',memory(a+offsets[f]+cell//(64//width)*8,8))[0]
            data[f]=(word>>((cell%(64//width))*width))&((1<<width)-1)
        return data
    chains={'global':ref(g['gc']['root'])}
    for t in tgs:
        for f in ['gcroot_pending','gcroot_pending_after_main']:
            chains[f"tid{t['tid']}-{f}"]=int(t[f],16)
    result={}
    for name,address in chains.items():
        start=address;walk=set();rows=[];hist=collections.Counter();arenas=collections.Counter()
        digest=hashlib.sha256();error=None
        while address and address not in walk and len(walk)<300000:
            try:
                raw=memory(address,16)
                successor=struct.unpack('<Q',raw[:8])[0]
                gct=raw[9];p=planes(address) if gct!=6 else {'thread_not_decoded':1}
                key=','.join([str(gct)]+[f'{k}:{v}' for k,v in p.items()])
                hist[key]+=1;arenas[hex(address&~65535)]+=1
                row={'address':hex(address),'next':hex(successor),'gct':gct,'planes':p}
                if len(rows)<8:rows.append(row)
                tail=row;walk.add(address)
                digest.update(struct.pack('<Q',address));address=successor
            except Exception as e:
                error=str(e);break
        result[name]={'start':hex(start),'end':hex(address),'count':len(walk),
                      'cycle':address in walk,'bound_exhausted':len(walk)==300000,
                      'histogram':dict(hist),'arenas':dict(arenas),
                      'ordered_address_sha256':digest.hexdigest(),
                      'first':rows,'last':tail if walk else None,'error':error}
    return result

def snapshot(where,tick):
    gp=gdb.parse_and_eval('sweep_probe_global')
    g=gp.dereference();gc=g['gc2']
    result={'where':where,'tick':tick,'g':hex(int(gp)),
            'threads':[(t.num,t.ptid,t.name,t.is_stopped()) for t in gdb.selected_inferior().threads()],
            'gc2':scalar_struct(gc),'gc':plain(g['gc']),
            'ssb_head':ssb_node(gc['ssb_head']),'ssb_drain':ssb_node(gc['ssb_drain'])}
    top=int(gc['grey_top']);bottom=int(gc['grey_bottom']);cap=int(gc['grey_capacity'])
    result['grey']=[hex(ref(gc['grey_stack'][i & (cap-1)]))
                    for i in range(top,min(bottom,top+1024))] if cap else []
    tgs={int(g['main_tg'])}
    for i in range(min(int(gc['n_workers']),2)):
        if int(gc['worker_tg'][i]):tgs.add(int(gc['worker_tg'][i]))
    p=int(gc['tg_list']);walk=set()
    while p and p not in walk and len(walk)<64:
        walk.add(p);tgs.add(p);p=int(ptr(p,'TGState')['next_tg'])
    result['tg_list_end']=hex(p)
    result['tgs']=[tg_state(t) for t in sorted(tgs)]
    objects=set(int(s,16) for s in result['grey'])
    for q in [result['ssb_head'],result['ssb_drain']]:
        for n in q['nodes']:objects.update(int(s,16) for s in n['slots'])
    for t in result['tgs']:
        objects.update(int(s,16) for s,_ in collections.Counter(t['private_ssb_slots']).most_common(12))
    tracked_objects.update(objects)
    result['objects']=[object_state(a) for a in sorted(objects | tracked_objects) if a][:256]
    if where=='native_return' and tick==575:
        result['root_chains']=root_chains(g,result['tgs'])
        result['lua_stack_functions']=lua_stack_functions(result['tgs'])
    result['backtraces']=gdb.execute('thread apply all bt 18',to_string=True)
    name=f'{len(seen):02d}-{where}-{tick}'
    (out/(name+'.json')).write_text(json.dumps(result,indent=2)+'\n')
    (out/(name+'.stacks')).write_text(result['backtraces'])
    print('SWEEP_SNAPSHOT '+name,flush=True)

class Checkpoint(gdb.Breakpoint):
    def stop(self):
        global pending
        try:
            where=gdb.parse_and_eval('where').string()
            tick=int(gdb.parse_and_eval('sweep_probe_ticks'))
            want=(where!='automatic_check' or tick in (100,298,496,568,569,572,576))
            key=(where,tick)
            if want and key not in seen:
                seen.add(key)
                pending = (where,tick)
                return True
        except Exception as e:
            (out/'errors.txt').open('a').write(str(e)+'\n')
            print('SWEEP_SNAPSHOT_ERROR '+str(e),flush=True)
        return False

class NativeReturn(gdb.Breakpoint):
    def stop(self):
        global pending
        try:
            tick=int(gdb.parse_and_eval('sweep_probe_ticks'))
            if tick not in (571,575):
                return False
            L=gdb.parse_and_eval('L')
            gp=gdb.parse_and_eval('sweep_probe_global')
            if int(L) != int(gp['main_tg']['thread_L']):
                return False
            key=('native_return',tick)
            if key in seen:
                return False
            seen.add(key)
            pending=key
            return True
        except Exception as e:
            (out/'native-errors.txt').open('a').write(str(e)+'\n')
            return False

gdb.execute('set pagination off')
gdb.execute('set print elements 32')
gdb.execute('set max-value-size unlimited')
gdb.execute('set confirm off')
Checkpoint('sweep_probe_checkpoint')
NativeReturn('lj_native_leave')
gdb.execute('set non-stop off')
gdb.execute('run')
while pending is not None:
    where,tick=pending
    pending=None
    try:
        assert all(t.is_stopped() for t in gdb.selected_inferior().threads())
        snapshot(where,tick)
    except Exception as e:
        import traceback
        (out/'errors.txt').open('a').write(traceback.format_exc()+'\n')
        print('SWEEP_SNAPSHOT_ERROR '+str(e),flush=True)
    gdb.execute('continue')
