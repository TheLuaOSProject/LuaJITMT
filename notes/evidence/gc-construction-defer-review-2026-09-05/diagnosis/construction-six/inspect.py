import gdb
import json

def emit(label, data):
    print('FUNC_DIAG ' + label + ' ' + json.dumps(data, sort_keys=True))

def field(v, name):
    try:
        x = v[name]
        return int(x)
    except Exception as e:
        return str(e)

def arena(p):
    address = int(p)
    a = gdb.Value(address & ~65535).cast(gdb.lookup_type('GCArena').pointer())
    i = (address & 65535) >> 4
    v = a.dereference()
    x = {'arena': int(a), 'cell': i}
    for name in ['block', 'mark', 'ready', 'late']:
        x[name] = (int(v[name][i >> 6]) >> (i & 63)) & 1
    for name in ['root', 'recovery', 'sweep']:
        x[name] = (int(v[name][i >> 5]) >> ((i & 31) * 2)) & 3
    x['lifetime'] = (int(v['lifetime'][i >> 4]) >> ((i & 15) * 4)) & 15
    hdr = v['hdr']
    for name in ['flags', 'owner_tid', 'reclaim_cell', 'retire_epoch', 'sweep_epoch', 'remote_active']:
        x[name] = field(hdr, name)
    return x

def snapshot(label):
    try:
        g = gdb.parse_and_eval('$diag_g').dereference()
        fields = ['phase', 'cycle', 'cycle_leader', 'mark_close_intent', 'mark_root_scanned',
                  'worker_active', 'assist_active', 'n_workers', 'hs_pending', 'hs_epoch',
                  'sweep_bridge_ready', 'sweep_root_scanned', 'sweep_grace_needed',
                  'ssb_head', 'ssb_drain', 'ssb_consumer_active', 'grey_top', 'grey_bottom',
                  'recovery_items', 'recovery_failed', 'table_rescan_pending',
                  'thread_scan_needscan_pending', 'sweep_owner_runs', 'sweep_owner_arenas',
                  'sweep_owner_live_cells', 'deferred_epoch', 'smr_reclaiming']
        emit(label + '-gc', {name: field(g['gc2'], name) for name in fields})
        fn = gdb.parse_and_eval('$diag_fn')
        uv = gdb.parse_and_eval('$diag_uv')
        emit(label + '-function', {'address': int(fn), **arena(fn)})
        if int(uv):
            emit(label + '-upvalue', {'address': int(uv), **arena(uv)})
        tg = g['main_tg'].dereference()
        emit(label + '-tg', {name: field(tg, name) for name in ['ssb_base', 'ssb_next', 'ssb_free', 'poll', 'reqmask']})
        alloc = tg['alloc']
        emit(label + '-alloc', {name: field(alloc, name) for name in ['prepare_epoch', 'sweep_epoch', 'huge_retire_cursor', 'huge_reclaim_cursor', 'huge_retire_done']})
        for name in ['needsweep', 'quarantine', 'owned']:
            p = alloc[name][0]
            emit(label + '-' + name, {'head': int(p), 'hdr': str(p.dereference()['hdr']) if int(p) else None})
    except Exception as e:
        emit(label + '-error', {'message': str(e)})

class Capture(gdb.Breakpoint):
    def stop(self):
        f = gdb.newest_frame()
        found = {}
        while f:
            name = f.name() or ''
            if name == 'func_finduv_nothrow':
                for k in ['L', 'g', 'uv']:
                    try: found[k] = f.read_var(k)
                    except Exception as e: emit('capture-var-error', {'frame': name, 'var': k, 'error': str(e)})
            if name == 'func_newL_gc_base':
                try:
                    value = f.read_var('fn')
                    if value.is_optimized_out and int(f.read_var('i')) == 0:
                        value = f.read_var('tail').cast(gdb.lookup_type('GCfunc').pointer())
                        emit('capture-fn-provenance', {'source': 'tail at i=0, before any pending-chain append; source equality tail=obj2gco(fn)'})
                    found['fn'] = value
                except Exception as e: emit('capture-var-error', {'frame': name, 'var': 'fn', 'error': str(e)})
            f = f.older()
        for k, v in found.items():
            gdb.set_convenience_variable('diag_' + k, v)
        emit('capture', {k: str(v) for k, v in found.items()})
        snapshot('entry')
        self.enabled = False
        print('FUNC_DIAG COLLECT_ENTRY_CAPTURED', flush=True)
        return False

class ConstructorReturn(gdb.FinishBreakpoint):
    def stop(self):
        gdb.set_convenience_variable('diag_fn', self.return_value)
        gdb.set_convenience_variable('diag_uv', gdb.Value(0).cast(gdb.lookup_type('GCupval').pointer()))
        snapshot('constructor-return')
        return False

class Constructor(gdb.Breakpoint):
    def stop(self):
        if int(gdb.parse_and_eval('func_test_finduv_collect_after')) != 1:
            return False
        L = gdb.newest_frame().read_var('L')
        gdb.set_convenience_variable('diag_L', L)
        gdb.set_convenience_variable('diag_g', gdb.parse_and_eval('(global_State *)$diag_L->glref.ptr64'))
        ConstructorReturn(gdb.newest_frame(), internal=True)
        self.enabled = False
        return False

class RootBlocker(gdb.Breakpoint):
    def stop(self):
        try:
            fn = int(gdb.parse_and_eval('$diag_fn'))
            f = gdb.newest_frame()
            if int(f.read_var('a')) != (fn & ~65535) or int(f.read_var('cell')) != ((fn & 65535) >> 4):
                return False
            emit('root-owner-refusal', {'cell': int(f.read_var('cell')), 'arena': int(f.read_var('a')), 'source_line': 'lj_gc.c:3024, ROOT_LINKING/UNLINKING branch'})
            snapshot('root-owner-refusal')
            self.enabled = False
        except Exception as e:
            emit('root-owner-break-error', {'error': str(e)})
            self.enabled = False
        return False

Constructor('func_newL_unlinked_nothrow')
RootBlocker('lj_gc.c:3024')

Capture('lj_func.c:86')
