[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".
0x00007fa69d99c7b9 in syscall () from /lib/x86_64-linux-gnu/libc.so.6

Thread 1 (Thread 0x7fa69d88d1c0 (LWP 116417) "strict-fnew-pre"):
#0  0x00007fa69d99c7b9 in syscall () from /lib/x86_64-linux-gnu/libc.so.6
#1  0x000055eb2ea26ed4 in lj_gc2_sweep_prepare_bridge_boundary.part ()
#2  0x000055eb2ea3a506 in lj_gc2_collect_active ()
#3  0x000055eb2ea94549 in api_gc_collect_cp ()
#4  0x000055eb2e9f985b in lj_vm_cpcall_asm ()
#5  0x000055eb2ea6a040 in lj_vm_cpcall ()
#6  0x000055eb2ea9ee26 in lua_gc ()
#7  0x000055eb2e9f2ae6 in test_active_black_direct_publishes_typed (L=L@entry=0x7fa69d8700a0, g=g@entry=0x7fa69d870150, tg=tg@entry=0x7fa69d872d40) at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:2967
#8  0x000055eb2e9eef64 in main () at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:3126
rax            0x0                 0
rbx            0x7fa69d870150      140353584169296
rcx            0x7fa69d99c7b9      140353585399737
rdx            0x7fffffff          2147483647
rsi            0x81                129
rdi            0x7fa69d871290      140353584173712
rbp            0x0                 0x0
rsp            0x7fff93b859c8      0x7fff93b859c8
r8             0x0                 0
r9             0x0                 0
r10            0x0                 0
r11            0x246               582
r12            0x0                 0
r13            0x7fa69d8708e0      140353584171232
r14            0x7fa69d872d40      140353584180544
r15            0x7fa69d870150      140353584169296
rip            0x7fa69d99c7b9      0x7fa69d99c7b9 <syscall+25>
eflags         0x246               [ PF ZF IF ]
cs             0x33                51
ss             0x2b                43
ds             0x0                 0
es             0x0                 0
fs             0x0                 0
gs             0x0                 0
fs_base        0x7fa69d88d1c0      140353584288192
gs_base        0x0                 0
[Inferior 1 (process 116417) detached]
