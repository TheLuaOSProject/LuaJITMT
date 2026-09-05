set pagination off
set confirm off
set print elements 12
break *lj_tab_next_rooted if ((global_State *)((lua_State *)$rdi)->glref.ptr64)->gc2.smr_reclaiming != 0
run
set $probe_L = (lua_State *)$rdi
set $probe_g = (global_State *)$probe_L->glref.ptr64
set $probe_tabroot = (TValue *)$rsi
set $probe_keyroot = (TValue *)$rdx
set $probe_outkey = (TValue *)$rcx
set $probe_outval = (TValue *)$r8
set $probe_table = (GCtab *)($probe_tabroot->u64 & 0x7fffffffffff)
printf "entry phase=%u smr=%u readers=%u native_gate=%u table_root=%p key_root=%p outkey=%p outval=%p\n", $probe_g->gc2.phase, $probe_g->gc2.smr_reclaiming, $probe_g->gc2.smr_readers, $probe_g->gc2.jit_phase_gate, $probe_tabroot, $probe_keyroot, $probe_outkey, $probe_outval
printf "source table_word=%#lx key_word=%#lx table=%p gct=%u asize=%u array=%p node=%p nilnode=%p sizeof_tab=%lu\n", $probe_tabroot->u64, $probe_keyroot->u64, $probe_table, $probe_table->gct, $probe_table->asize, $probe_table->array.ptr64, $probe_table->node.ptr64, &$probe_g->nilnode, sizeof(GCtab)
printf "array_words\n"
x/6gx $probe_table->array.ptr64
thread apply all bt 7
tbreak *lj_gc2_smr_read_try
continue
printf "smr_try entry g=%p phase=%u smr=%u readers=%u native_gate=%u\n", $rdi, $probe_g->gc2.phase, $probe_g->gc2.smr_reclaiming, $probe_g->gc2.smr_readers, $probe_g->gc2.jit_phase_gate
bt 3
finish
printf "smr_try result=%d phase=%u smr=%u readers=%u native_gate=%u\n", $eax, $probe_g->gc2.phase, $probe_g->gc2.smr_reclaiming, $probe_g->gc2.smr_readers, $probe_g->gc2.jit_phase_gate
quit
