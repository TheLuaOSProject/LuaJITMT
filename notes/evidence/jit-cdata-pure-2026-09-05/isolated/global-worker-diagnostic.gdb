[New LWP 199987]
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".
0x000055ddc2d05d5f in gc2_small_containing_start (a=a@entry=0x7fa0742b0000, cell=cell@entry=616, startp=startp@entry=0x7ffe491116e4) at lj_gc2.c:17119
17119	    uint64_t bits = la_load64_acq(&a->block[wi]) |

Thread 2 (Thread 0x7fa07388d6c0 (LWP 199987) "base-normal-glo"):
#0  0x00007fa0746167b9 in syscall () from /lib/x86_64-linux-gnu/libc.so.6
#1  0x000055ddc2d29c0c in la_futex_wait (p=<optimized out>, val=<optimized out>, ns=<optimized out>) at /tmp/lj-premt-cdata-hoist-20260905-oa96m15y/base-normal/src/lj_atomic.h:163
#2  gc2_worker_wake_futex_wait (g=<optimized out>, wake=<optimized out>, timeout_ns=<optimized out>) at /tmp/lj-premt-cdata-hoist-20260905-oa96m15y/base-normal/src/lj_obj.h:5089
#3  gc2_worker_deferred_backoff (g=<optimized out>) at lj_gc2.c:2752
#4  gc2_worker_main (arg=0x7fa06c000b90) at lj_gc2.c:2797
#5  0x00007fa07459ab7b in ?? () from /lib/x86_64-linux-gnu/libc.so.6
#6  0x00007fa0746187f8 in ?? () from /lib/x86_64-linux-gnu/libc.so.6

Thread 1 (Thread 0x7fa0745071c0 (LWP 199985) "base-normal-glo"):
#0  0x000055ddc2d05d5f in gc2_small_containing_start (a=a@entry=0x7fa0742b0000, cell=cell@entry=616, startp=startp@entry=0x7ffe491116e4) at lj_gc2.c:17119
#1  0x000055ddc2d0e544 in gc2_small_candidate_admit (g=g@entry=0x7fa0744f0130, o=o@entry=0x7fa0742b2680, a=a@entry=0x7fa0742b0000, expected_gct=expected_gct@entry=0, basep=basep@entry=0x7ffe49111758, startp=startp@entry=0x7ffe49111760, gctp=0x7ffe49111754, scope=0x7ffe49111820) at lj_gc2.c:17311
#2  0x000055ddc2d11a05 in gc2_observed_obj_status_scoped_impl (g=g@entry=0x7fa0744f0130, o=o@entry=0x7fa0742b2680, gctp=gctp@entry=0x7ffe491117ec, scope=scope@entry=0x7ffe49111820, startp=0x0) at lj_gc2.c:10673
#3  0x000055ddc2d11c08 in gc2_tv_admit_scoped_impl (g=0x7fa0744f0130, tv=0x7ffe491118d8, tv@entry=0x7ffe49111748, scope=scope@entry=0x7ffe49111820, startp=startp@entry=0x0) at lj_gc2.c:7221
#4  0x000055ddc2d1f855 in gc2_tv_admit_scoped (g=<optimized out>, tv=0x7ffe49111748, scope=0x7ffe49111820) at lj_gc2.c:7248
#5  lj_gc2_tv_lease_acquire (g=<optimized out>, tv=0x7ffe49111748, lease=0x7ffe49111900) at lj_gc2.c:16660
#6  lj_gc2_tv_lease_acquire (g=<optimized out>, tv=tv@entry=0x7ffe491118d8, lease=lease@entry=0x7ffe49111900) at lj_gc2.c:16650
#7  0x000055ddc2d3cbb2 in tab_gettv_rooted_impl (L=L@entry=0x7fa0744f0080, tabroot=tabroot@entry=0x7fa0744f79e8, trusted_t=trusted_t@entry=0x0, key=key@entry=0x7fa0744f79f0, outroot=outroot@entry=0x7fa0744f79f8) at lj_tab.c:5150
#8  0x000055ddc2d44afd in lj_tab_gettv_rooted (L=L@entry=0x7fa0744f0080, tabroot=tabroot@entry=0x7fa0744f79e8, key=key@entry=0x7fa0744f79f0, outroot=outroot@entry=0x7fa0744f79f8) at lj_tab.c:5256
#9  0x000055ddc2d51121 in meta_tget_rooted_mode (L=0x7fa0744f0080, o=<optimized out>, k=<optimized out>, out=<optimized out>, funcenv=<optimized out>) at lj_meta.c:789
#10 0x000055ddc2dc6cf0 in lj_vmeta_ggets ()
#11 0x000055ddc2d569a7 in lj_vm_pcall_unwind (L=L@entry=0x7fa0744f0080, base=0x7fa074194600, nres1=nres1@entry=1, ef=ef@entry=0) at lj_state.c:130
#12 0x000055ddc2d85981 in lua_pcall (L=L@entry=0x7fa0744f0080, nargs=nargs@entry=0, nresults=nresults@entry=0, errfunc=errfunc@entry=0) at lj_api.c:2797
#13 0x000055ddc2d04059 in ljt_lua_pcall (nargs=0, nresults=0, what=0x55ddc2e3008b "phase continuation", L=0x7fa0744f0080) at /tmp/lj-premt-cdata-hoist-20260905-oa96m15y/base-normal/tests/lib/lua_fixture_helpers.h:50
#14 main () at /tmp/lj-premt-cdata-hoist-20260905-oa96m15y/global-worker.c:59
[Inferior 1 (process 199985) detached]
