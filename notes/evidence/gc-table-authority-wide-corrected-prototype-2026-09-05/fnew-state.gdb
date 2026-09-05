[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".
0x00005634bc8dd5f4 in lj_gc_reclaim_gc2_arena ()
#0  0x00005634bc8dd5f4 in lj_gc_reclaim_gc2_arena ()
#1  0x00005634bc8ee717 in lj_gc2_sweep_owner_progress ()
#2  0x00005634bc91158e in gc2_worker_drain_inner ()
#3  0x00005634bc915548 in lj_gc2_collect_active ()
#4  0x00005634bc96f549 in api_gc_collect_cp ()
#5  0x00005634bc8d485b in lj_vm_cpcall_asm ()
#6  0x00005634bc945040 in lj_vm_cpcall ()
#7  0x00005634bc979e26 in lua_gc ()
#8  0x00005634bc8cdae6 in test_active_black_direct_publishes_typed (L=L@entry=0x7f9acd7c00a0, g=g@entry=0x7f9acd7c0150, tg=tg@entry=0x7f9acd7c2d40) at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:2967
#9  0x00005634bc8c9f64 in main () at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:3126
#7  0x00005634bc979e26 in lua_gc ()
No symbol "g" in current context.
No symbol "tg" in current context.
No symbol "tg" in current context.
[Inferior 1 (process 118470) detached]
