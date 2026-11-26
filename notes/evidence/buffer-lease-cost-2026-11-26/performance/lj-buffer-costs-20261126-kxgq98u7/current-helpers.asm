
/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000ca6d0 <bufx_pin_try>:
   ca6d0:	41 57                	push   r15
   ca6d2:	49 89 cf             	mov    r15,rcx
   ca6d5:	49 89 fa             	mov    r10,rdi
   ca6d8:	b9 0e 00 00 00       	mov    ecx,0xe
   ca6dd:	41 56                	push   r14
   ca6df:	4c 89 ff             	mov    rdi,r15
   ca6e2:	41 55                	push   r13
   ca6e4:	45 31 ed             	xor    r13d,r13d
   ca6e7:	41 54                	push   r12
   ca6e9:	4c 89 e8             	mov    rax,r13
   ca6ec:	55                   	push   rbp
   ca6ed:	53                   	push   rbx
   ca6ee:	4c 89 eb             	mov    rbx,r13
   ca6f1:	48 83 ec 28          	sub    rsp,0x28
   ca6f5:	f3 48 ab             	rep stos QWORD PTR es:[rdi],rax
   ca6f8:	89 14 24             	mov    DWORD PTR [rsp],edx
   ca6fb:	4c 89 ea             	mov    rdx,r13
   ca6fe:	f0 48 0f c7 0e       	lock cmpxchg16b OWORD PTR [rsi]
   ca703:	49 89 07             	mov    QWORD PTR [r15],rax
   ca706:	49 89 57 08          	mov    QWORD PTR [r15+0x8],rdx
   ca70a:	49 89 47 10          	mov    QWORD PTR [r15+0x10],rax
   ca70e:	48 85 c0             	test   rax,rax
   ca711:	75 1d                	jne    ca730 <bufx_pin_try+0x60>
   ca713:	48 83 c4 28          	add    rsp,0x28
   ca717:	b8 01 00 00 00       	mov    eax,0x1
   ca71c:	5b                   	pop    rbx
   ca71d:	5d                   	pop    rbp
   ca71e:	41 5c                	pop    r12
   ca720:	41 5d                	pop    r13
   ca722:	41 5e                	pop    r14
   ca724:	41 5f                	pop    r15
   ca726:	c3                   	ret
   ca727:	66 0f 1f 84 00 00 00 00 00 	nop    WORD PTR [rax+rax*1+0x0]
   ca730:	49 8b 7a 10          	mov    rdi,QWORD PTR [r10+0x10]
   ca734:	4d 8d 77 18          	lea    r14,[r15+0x18]
   ca738:	45 31 c0             	xor    r8d,r8d
   ca73b:	49 89 f4             	mov    r12,rsi
   ca73e:	4c 89 f1             	mov    rcx,r14
   ca741:	ba 30 00 00 00       	mov    edx,0x30
   ca746:	48 89 c6             	mov    rsi,rax
   ca749:	4c 89 54 24 08       	mov    QWORD PTR [rsp+0x8],r10
   ca74e:	48 89 c5             	mov    rbp,rax
   ca751:	e8 1a 06 ff ff       	call   bad70 <lj_gc2_storage_lease_try>
   ca756:	4c 8b 54 24 08       	mov    r10,QWORD PTR [rsp+0x8]
   ca75b:	44 8b 0c 24          	mov    r9d,DWORD PTR [rsp]
   ca75f:	85 c0                	test   eax,eax
   ca761:	74 45                	je     ca7a8 <bufx_pin_try+0xd8>
   ca763:	4c 89 e8             	mov    rax,r13
   ca766:	4c 89 ea             	mov    rdx,r13
   ca769:	4c 89 eb             	mov    rbx,r13
   ca76c:	4c 89 e9             	mov    rcx,r13
   ca76f:	f0 49 0f c7 0c 24    	lock cmpxchg16b OWORD PTR [r12]
   ca775:	4d 8d 6f 40          	lea    r13,[r15+0x40]
   ca779:	49 39 07             	cmp    QWORD PTR [r15],rax
   ca77c:	75 06                	jne    ca784 <bufx_pin_try+0xb4>
   ca77e:	49 39 57 08          	cmp    QWORD PTR [r15+0x8],rdx
   ca782:	74 2c                	je     ca7b0 <bufx_pin_try+0xe0>
   ca784:	4c 89 ef             	mov    rdi,r13
   ca787:	e8 84 01 ff ff       	call   ba910 <lj_gc2_lease_release>
   ca78c:	4c 89 f7             	mov    rdi,r14
   ca78f:	e8 7c 01 ff ff       	call   ba910 <lj_gc2_lease_release>
   ca794:	48 83 c4 28          	add    rsp,0x28
   ca798:	31 c0                	xor    eax,eax
   ca79a:	5b                   	pop    rbx
   ca79b:	5d                   	pop    rbp
   ca79c:	41 5c                	pop    r12
   ca79e:	41 5d                	pop    r13
   ca7a0:	41 5e                	pop    r14
   ca7a2:	41 5f                	pop    r15
   ca7a4:	c3                   	ret
   ca7a5:	0f 1f 00             	nop    DWORD PTR [rax]
   ca7a8:	4d 8d 6f 40          	lea    r13,[r15+0x40]
   ca7ac:	eb d6                	jmp    ca784 <bufx_pin_try+0xb4>
   ca7ae:	66 90                	xchg   ax,ax
   ca7b0:	8b 4d 28             	mov    ecx,DWORD PTR [rbp+0x28]
   ca7b3:	49 8b 47 08          	mov    rax,QWORD PTR [r15+0x8]
   ca7b7:	48 89 c2             	mov    rdx,rax
   ca7ba:	48 c1 ea 20          	shr    rdx,0x20
   ca7be:	39 ca                	cmp    edx,ecx
   ca7c0:	73 c2                	jae    ca784 <bufx_pin_try+0xb4>
   ca7c2:	45 85 c9             	test   r9d,r9d
   ca7c5:	0f 84 48 ff ff ff    	je     ca713 <bufx_pin_try+0x43>
   ca7cb:	48 8b 4d 10          	mov    rcx,QWORD PTR [rbp+0x10]
   ca7cf:	48 85 c9             	test   rcx,rcx
   ca7d2:	0f 84 3b ff ff ff    	je     ca713 <bufx_pin_try+0x43>
   ca7d8:	39 c2                	cmp    edx,eax
   ca7da:	0f 84 33 ff ff ff    	je     ca713 <bufx_pin_try+0x43>
   ca7e0:	8b 45 24             	mov    eax,DWORD PTR [rbp+0x24]
   ca7e3:	f7 d0                	not    eax
   ca7e5:	48 c1 e0 2f          	shl    rax,0x2f
   ca7e9:	48 09 c1             	or     rcx,rax
   ca7ec:	48 89 4c 24 18       	mov    QWORD PTR [rsp+0x18],rcx
   ca7f1:	83 7d 24 04          	cmp    DWORD PTR [rbp+0x24],0x4
   ca7f5:	74 43                	je     ca83a <bufx_pin_try+0x16a>
   ca7f7:	49 8b 7a 10          	mov    rdi,QWORD PTR [r10+0x10]
   ca7fb:	48 8d 74 24 18       	lea    rsi,[rsp+0x18]
   ca800:	4c 89 ea             	mov    rdx,r13
   ca803:	e8 28 00 ff ff       	call   ba830 <lj_gc2_tv_lease_acquire>
   ca808:	83 f8 01             	cmp    eax,0x1
   ca80b:	0f 85 73 ff ff ff    	jne    ca784 <bufx_pin_try+0xb4>
   ca811:	31 c0                	xor    eax,eax
   ca813:	48 89 c2             	mov    rdx,rax
   ca816:	48 89 c3             	mov    rbx,rax
   ca819:	48 89 c1             	mov    rcx,rax
   ca81c:	f0 49 0f c7 0c 24    	lock cmpxchg16b OWORD PTR [r12]
   ca822:	49 39 57 08          	cmp    QWORD PTR [r15+0x8],rdx
   ca826:	0f 85 58 ff ff ff    	jne    ca784 <bufx_pin_try+0xb4>
   ca82c:	49 39 07             	cmp    QWORD PTR [r15],rax
   ca82f:	0f 85 4f ff ff ff    	jne    ca784 <bufx_pin_try+0xb4>
   ca835:	e9 d9 fe ff ff       	jmp    ca713 <bufx_pin_try+0x43>
   ca83a:	49 8b 7a 10          	mov    rdi,QWORD PTR [r10+0x10]
   ca83e:	48 8b 75 10          	mov    rsi,QWORD PTR [rbp+0x10]
   ca842:	45 31 c9             	xor    r9d,r9d
   ca845:	4d 89 e8             	mov    r8,r13
   ca848:	b9 04 00 00 00       	mov    ecx,0x4
   ca84d:	ba 30 00 00 00       	mov    edx,0x30
   ca852:	4c 89 14 24          	mov    QWORD PTR [rsp],r10
   ca856:	e8 45 01 ff ff       	call   ba9a0 <lj_gc2_small_lease_try>
   ca85b:	4c 8b 14 24          	mov    r10,QWORD PTR [rsp]
   ca85f:	85 c0                	test   eax,eax
   ca861:	74 94                	je     ca7f7 <bufx_pin_try+0x127>
   ca863:	eb ac                	jmp    ca811 <bufx_pin_try+0x141>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cbcd0 <lj_bufx_putstr>:
   cbcd0:	41 56                	push   r14
   cbcd2:	41 55                	push   r13
   cbcd4:	41 54                	push   r12
   cbcd6:	49 89 f4             	mov    r12,rsi
   cbcd9:	48 83 ee 30          	sub    rsi,0x30
   cbcdd:	55                   	push   rbp
   cbcde:	48 89 d5             	mov    rbp,rdx
   cbce1:	ba f3 ff ff ff       	mov    edx,0xfffffff3
   cbce6:	53                   	push   rbx
   cbce7:	48 89 fb             	mov    rbx,rdi
   cbcea:	e8 a1 ed ff ff       	call   caa90 <bufx_anchor>
   cbcef:	ba fb ff ff ff       	mov    edx,0xfffffffb
   cbcf4:	48 89 ee             	mov    rsi,rbp
   cbcf7:	48 89 df             	mov    rdi,rbx
   cbcfa:	41 89 c5             	mov    r13d,eax
   cbcfd:	e8 8e ed ff ff       	call   caa90 <bufx_anchor>
   cbd02:	8b 4d 14             	mov    ecx,DWORD PTR [rbp+0x14]
   cbd05:	48 89 df             	mov    rdi,rbx
   cbd08:	48 8d 55 30          	lea    rdx,[rbp+0x30]
   cbd0c:	4c 89 e6             	mov    rsi,r12
   cbd0f:	41 89 c6             	mov    r14d,eax
   cbd12:	e8 39 fe ff ff       	call   cbb50 <lj_bufx_append>
   cbd17:	48 8b 7b 78          	mov    rdi,QWORD PTR [rbx+0x78]
   cbd1b:	48 85 ff             	test   rdi,rdi
   cbd1e:	74 28                	je     cbd48 <lj_bufx_putstr+0x78>
   cbd20:	44 89 f6             	mov    esi,r14d
   cbd23:	e8 18 70 f5 ff       	call   22d40 <lj_tg_root_anchor_pop>
   cbd28:	48 8b 7b 78          	mov    rdi,QWORD PTR [rbx+0x78]
   cbd2c:	48 85 ff             	test   rdi,rdi
   cbd2f:	74 2f                	je     cbd60 <lj_bufx_putstr+0x90>
   cbd31:	5b                   	pop    rbx
   cbd32:	44 89 ee             	mov    esi,r13d
   cbd35:	5d                   	pop    rbp
   cbd36:	41 5c                	pop    r12
   cbd38:	41 5d                	pop    r13
   cbd3a:	41 5e                	pop    r14
   cbd3c:	e9 ff 6f f5 ff       	jmp    22d40 <lj_tg_root_anchor_pop>
   cbd41:	0f 1f 80 00 00 00 00 	nop    DWORD PTR [rax+0x0]
   cbd48:	48 8b 7b 10          	mov    rdi,QWORD PTR [rbx+0x10]
   cbd4c:	e8 8f 9e f5 ff       	call   25be0 <lj_thr_get_tg_fallback>
   cbd51:	48 89 c7             	mov    rdi,rax
   cbd54:	eb ca                	jmp    cbd20 <lj_bufx_putstr+0x50>
   cbd56:	66 2e 0f 1f 84 00 00 00 00 00 	cs nop WORD PTR [rax+rax*1+0x0]
   cbd60:	48 8b 7b 10          	mov    rdi,QWORD PTR [rbx+0x10]
   cbd64:	e8 77 9e f5 ff       	call   25be0 <lj_thr_get_tg_fallback>
   cbd69:	5b                   	pop    rbx
   cbd6a:	44 89 ee             	mov    esi,r13d
   cbd6d:	5d                   	pop    rbp
   cbd6e:	48 89 c7             	mov    rdi,rax
   cbd71:	41 5c                	pop    r12
   cbd73:	41 5d                	pop    r13
   cbd75:	41 5e                	pop    r14
   cbd77:	e9 c4 6f f5 ff       	jmp    22d40 <lj_tg_root_anchor_pop>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cb920 <lj_bufx_reset>:
   cb920:	41 56                	push   r14
   cb922:	49 be 00 00 00 00 ff ff ff ff 	movabs r14,0xffffffff00000000
   cb92c:	41 55                	push   r13
   cb92e:	41 54                	push   r12
   cb930:	49 89 f4             	mov    r12,rsi
   cb933:	55                   	push   rbp
   cb934:	48 89 fd             	mov    rbp,rdi
   cb937:	53                   	push   rbx
   cb938:	48 83 ec 70          	sub    rsp,0x70
   cb93c:	49 89 e5             	mov    r13,rsp
   cb93f:	eb 0f                	jmp    cb950 <lj_bufx_reset+0x30>
   cb941:	0f 1f 80 00 00 00 00 	nop    DWORD PTR [rax+0x0]
   cb948:	48 89 ef             	mov    rdi,rbp
   cb94b:	e8 e0 6a 00 00       	call   d2430 <lj_tab_wait_l>
   cb950:	31 d2                	xor    edx,edx
   cb952:	4c 89 e9             	mov    rcx,r13
   cb955:	4c 89 e6             	mov    rsi,r12
   cb958:	48 89 ef             	mov    rdi,rbp
   cb95b:	e8 70 ed ff ff       	call   ca6d0 <bufx_pin_try>
   cb960:	85 c0                	test   eax,eax
   cb962:	74 e4                	je     cb948 <lj_bufx_reset+0x28>
   cb964:	48 8b 74 24 10       	mov    rsi,QWORD PTR [rsp+0x10]
   cb969:	48 85 f6             	test   rsi,rsi
   cb96c:	74 3f                	je     cb9ad <lj_bufx_reset+0x8d>
   cb96e:	48 83 7e 10 00       	cmp    QWORD PTR [rsi+0x10],0x0
   cb973:	48 8b 54 24 08       	mov    rdx,QWORD PTR [rsp+0x8]
   cb978:	48 8b 04 24          	mov    rax,QWORD PTR [rsp]
   cb97c:	74 42                	je     cb9c0 <lj_bufx_reset+0xa0>
   cb97e:	31 c9                	xor    ecx,ecx
   cb980:	48 89 cb             	mov    rbx,rcx
   cb983:	f0 49 0f c7 0c 24    	lock cmpxchg16b OWORD PTR [r12]
   cb989:	75 50                	jne    cb9db <lj_bufx_reset+0xbb>
   cb98b:	48 8b 7d 10          	mov    rdi,QWORD PTR [rbp+0x10]
   cb98f:	ba 30 00 00 00       	mov    edx,0x30
   cb994:	e8 f7 1e fd ff       	call   9d890 <lj_mem_free>
   cb999:	48 8d 7c 24 40       	lea    rdi,[rsp+0x40]
   cb99e:	e8 6d ef fe ff       	call   ba910 <lj_gc2_lease_release>
   cb9a3:	48 8d 7c 24 18       	lea    rdi,[rsp+0x18]
   cb9a8:	e8 63 ef fe ff       	call   ba910 <lj_gc2_lease_release>
   cb9ad:	48 83 c4 70          	add    rsp,0x70
   cb9b1:	5b                   	pop    rbx
   cb9b2:	5d                   	pop    rbp
   cb9b3:	41 5c                	pop    r12
   cb9b5:	41 5d                	pop    r13
   cb9b7:	41 5e                	pop    r14
   cb9b9:	c3                   	ret
   cb9ba:	66 0f 1f 44 00 00    	nop    WORD PTR [rax+rax*1+0x0]
   cb9c0:	48 89 d1             	mov    rcx,rdx
   cb9c3:	48 89 d7             	mov    rdi,rdx
   cb9c6:	48 89 f3             	mov    rbx,rsi
   cb9c9:	4c 21 f1             	and    rcx,r14
   cb9cc:	48 c1 ef 20          	shr    rdi,0x20
   cb9d0:	48 09 f9             	or     rcx,rdi
   cb9d3:	f0 49 0f c7 0c 24    	lock cmpxchg16b OWORD PTR [r12]
   cb9d9:	74 19                	je     cb9f4 <lj_bufx_reset+0xd4>
   cb9db:	48 8d 7c 24 40       	lea    rdi,[rsp+0x40]
   cb9e0:	e8 2b ef fe ff       	call   ba910 <lj_gc2_lease_release>
   cb9e5:	48 8d 7c 24 18       	lea    rdi,[rsp+0x18]
   cb9ea:	e8 21 ef fe ff       	call   ba910 <lj_gc2_lease_release>
   cb9ef:	e9 5c ff ff ff       	jmp    cb950 <lj_bufx_reset+0x30>
   cb9f4:	48 8d 7c 24 40       	lea    rdi,[rsp+0x40]
   cb9f9:	e8 12 ef fe ff       	call   ba910 <lj_gc2_lease_release>
   cb9fe:	48 8d 7c 24 18       	lea    rdi,[rsp+0x18]
   cba03:	e8 08 ef fe ff       	call   ba910 <lj_gc2_lease_release>
   cba08:	48 83 c4 70          	add    rsp,0x70
   cba0c:	5b                   	pop    rbx
   cba0d:	5d                   	pop    rbp
   cba0e:	41 5c                	pop    r12
   cba10:	41 5d                	pop    r13
   cba12:	41 5e                	pop    r14
   cba14:	c3                   	ret

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cbf90 <lj_bufx_get>:
   cbf90:	48 83 ec 28          	sub    rsp,0x28
   cbf94:	41 b8 01 00 00 00    	mov    r8d,0x1
   cbf9a:	48 89 e1             	mov    rcx,rsp
   cbf9d:	e8 ae f2 ff ff       	call   cb250 <bufx_string>
   cbfa2:	48 83 c4 28          	add    rsp,0x28
   cbfa6:	c3                   	ret

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000ba9a0 <lj_gc2_small_lease_try>:
   ba9a0:	4d 85 c0             	test   r8,r8
   ba9a3:	74 7b                	je     baa20 <lj_gc2_small_lease_try+0x80>
   ba9a5:	41 57                	push   r15
   ba9a7:	66 0f ef c0          	pxor   xmm0,xmm0
   ba9ab:	48 89 f8             	mov    rax,rdi
   ba9ae:	41 56                	push   r14
   ba9b0:	41 55                	push   r13
   ba9b2:	41 54                	push   r12
   ba9b4:	41 89 cc             	mov    r12d,ecx
   ba9b7:	55                   	push   rbp
   ba9b8:	48 89 d5             	mov    rbp,rdx
   ba9bb:	53                   	push   rbx
   ba9bc:	48 89 f3             	mov    rbx,rsi
   ba9bf:	48 83 ec 68          	sub    rsp,0x68
   ba9c3:	49 c7 40 20 00 00 00 00 	mov    QWORD PTR [r8+0x20],0x0
   ba9cb:	41 0f 11 00          	movups XMMWORD PTR [r8],xmm0
   ba9cf:	41 0f 11 40 10       	movups XMMWORD PTR [r8+0x10],xmm0
   ba9d4:	4d 85 c9             	test   r9,r9
   ba9d7:	74 07                	je     ba9e0 <lj_gc2_small_lease_try+0x40>
   ba9d9:	49 c7 01 00 00 00 00 	mov    QWORD PTR [r9],0x0
   ba9e0:	48 85 c0             	test   rax,rax
   ba9e3:	74 24                	je     baa09 <lj_gc2_small_lease_try+0x69>
   ba9e5:	48 85 db             	test   rbx,rbx
   ba9e8:	74 1f                	je     baa09 <lj_gc2_small_lease_try+0x69>
   ba9ea:	48 8d 55 ff          	lea    rdx,[rbp-0x1]
   ba9ee:	48 81 fa ff 3f 00 00 	cmp    rdx,0x3fff
   ba9f5:	77 12                	ja     baa09 <lj_gc2_small_lease_try+0x69>
   ba9f7:	48 89 da             	mov    rdx,rbx
   ba9fa:	48 89 d9             	mov    rcx,rbx
   ba9fd:	48 c1 ea 2f          	shr    rdx,0x2f
   baa01:	83 e1 0f             	and    ecx,0xf
   baa04:	48 09 ca             	or     rdx,rcx
   baa07:	74 1f                	je     baa28 <lj_gc2_small_lease_try+0x88>
   baa09:	31 c0                	xor    eax,eax
   baa0b:	48 83 c4 68          	add    rsp,0x68
   baa0f:	5b                   	pop    rbx
   baa10:	5d                   	pop    rbp
   baa11:	41 5c                	pop    r12
   baa13:	41 5d                	pop    r13
   baa15:	41 5e                	pop    r14
   baa17:	41 5f                	pop    r15
   baa19:	c3                   	ret
   baa1a:	66 0f 1f 44 00 00    	nop    WORD PTR [rax+rax*1+0x0]
   baa20:	31 c0                	xor    eax,eax
   baa22:	c3                   	ret
   baa23:	0f 1f 44 00 00       	nop    DWORD PTR [rax+rax*1+0x0]
   baa28:	8b 50 10             	mov    edx,DWORD PTR [rax+0x10]
   baa2b:	85 d2                	test   edx,edx
   baa2d:	74 da                	je     baa09 <lj_gc2_small_lease_try+0x69>
   baa2f:	45 85 e4             	test   r12d,r12d
   baa32:	41 8d 54 24 f5       	lea    edx,[r12-0xb]
   baa37:	0f 95 c1             	setne  cl
   baa3a:	83 fa 01             	cmp    edx,0x1
   baa3d:	76 0e                	jbe    baa4d <lj_gc2_small_lease_try+0xad>
   baa3f:	84 c9                	test   cl,cl
   baa41:	74 0a                	je     baa4d <lj_gc2_small_lease_try+0xad>
   baa43:	41 8d 54 24 fc       	lea    edx,[r12-0x4]
   baa48:	83 fa 01             	cmp    edx,0x1
   baa4b:	77 bc                	ja     baa09 <lj_gc2_small_lease_try+0x69>
   baa4d:	4c 89 4c 24 18       	mov    QWORD PTR [rsp+0x18],r9
   baa52:	4c 89 44 24 10       	mov    QWORD PTR [rsp+0x10],r8
   baa57:	88 4c 24 0c          	mov    BYTE PTR [rsp+0xc],cl
   baa5b:	48 8b b8 88 0f 00 00 	mov    rdi,QWORD PTR [rax+0xf88]
   baa62:	48 85 ff             	test   rdi,rdi
   baa65:	74 a2                	je     baa09 <lj_gc2_small_lease_try+0x69>
   baa67:	49 89 df             	mov    r15,rbx
   baa6a:	49 c1 ef 04          	shr    r15,0x4
   baa6e:	45 89 fd             	mov    r13d,r15d
   baa71:	41 81 e5 ff 0f 00 00 	and    r13d,0xfff
   baa78:	41 8d 85 98 fd ff ff 	lea    eax,[r13-0x268]
   baa7f:	3d 97 0d 00 00       	cmp    eax,0xd97
   baa84:	77 83                	ja     baa09 <lj_gc2_small_lease_try+0x69>
   baa86:	49 89 de             	mov    r14,rbx
   baa89:	66 0f ef c0          	pxor   xmm0,xmm0
   baa8d:	48 8d 54 24 20       	lea    rdx,[rsp+0x20]
   baa92:	48 c7 44 24 30 00 00 00 00 	mov    QWORD PTR [rsp+0x30],0x0
   baa9b:	66 45 31 f6          	xor    r14w,r14w
   baa9f:	c7 44 24 38 00 00 00 00 	mov    DWORD PTR [rsp+0x38],0x0
   baaa7:	4c 89 f6             	mov    rsi,r14
   baaaa:	0f 29 44 24 40       	movaps XMMWORD PTR [rsp+0x40],xmm0
   baaaf:	48 c7 44 24 50 00 00 00 00 	mov    QWORD PTR [rsp+0x50],0x0
   baab8:	e8 83 df f7 ff       	call   38a40 <lj_arena_hugetab_observe_enter>
   baabd:	89 44 24 38          	mov    DWORD PTR [rsp+0x38],eax
   baac1:	85 c0                	test   eax,eax
   baac3:	0f 84 40 ff ff ff    	je     baa09 <lj_gc2_small_lease_try+0x69>
   baac9:	4c 89 74 24 30       	mov    QWORD PTR [rsp+0x30],r14
   baace:	f0 48 83 0c 24 00    	lock or QWORD PTR [rsp],0x0
   baad4:	48 81 7c 24 20 00 00 01 00 	cmp    QWORD PTR [rsp+0x20],0x10000
   baadd:	0f b6 4c 24 0c       	movzx  ecx,BYTE PTR [rsp+0xc]
   baae2:	4c 8b 44 24 10       	mov    r8,QWORD PTR [rsp+0x10]
   baae7:	4c 8b 4c 24 18       	mov    r9,QWORD PTR [rsp+0x18]
   baaec:	74 33                	je     bab21 <lj_gc2_small_lease_try+0x181>
   baaee:	8b 44 24 38          	mov    eax,DWORD PTR [rsp+0x38]
   baaf2:	3d ce bc b8 ff       	cmp    eax,0xffb8bcce
   baaf7:	0f 84 63 01 00 00    	je     bac60 <lj_gc2_small_lease_try+0x2c0>
   baafd:	48 8b 7c 24 30       	mov    rdi,QWORD PTR [rsp+0x30]
   bab02:	48 85 ff             	test   rdi,rdi
   bab05:	0f 84 fe fe ff ff    	je     baa09 <lj_gc2_small_lease_try+0x69>
   bab0b:	83 e8 01             	sub    eax,0x1
   bab0e:	83 f8 02             	cmp    eax,0x2
   bab11:	0f 87 f2 fe ff ff    	ja     baa09 <lj_gc2_small_lease_try+0x69>
   bab17:	e8 e4 2e f7 ff       	call   2da00 <lj_arena_rescue_leave>
   bab1c:	e9 e8 fe ff ff       	jmp    baa09 <lj_gc2_small_lease_try+0x69>
   bab21:	41 8b 06             	mov    eax,DWORD PTR [r14]
   bab24:	83 e0 01             	and    eax,0x1
   bab27:	38 c1                	cmp    cl,al
   bab29:	75 c3                	jne    baaee <lj_gc2_small_lease_try+0x14e>
   bab2b:	45 85 e4             	test   r12d,r12d
   bab2e:	74 34                	je     bab64 <lj_gc2_small_lease_try+0x1c4>
   bab30:	48 81 fb ff ff 00 00 	cmp    rbx,0xffff
   bab37:	76 b5                	jbe    baaee <lj_gc2_small_lease_try+0x14e>
   bab39:	44 89 e8             	mov    eax,r13d
   bab3c:	42 8d 0c ad 00 00 00 00 	lea    ecx,[r13*4+0x0]
   bab44:	ba a6 01 00 00       	mov    edx,0x1a6
   bab49:	c1 e8 04             	shr    eax,0x4
   bab4c:	83 e1 3c             	and    ecx,0x3c
   bab4f:	05 10 02 00 00       	add    eax,0x210
   bab54:	49 8b 04 c6          	mov    rax,QWORD PTR [r14+rax*8]
   bab58:	48 d3 e8             	shr    rax,cl
   bab5b:	83 e0 0f             	and    eax,0xf
   bab5e:	48 0f a3 c2          	bt     rdx,rax
   bab62:	73 8a                	jae    baaee <lj_gc2_small_lease_try+0x14e>
   bab64:	44 89 e8             	mov    eax,r13d
   bab67:	44 89 fe             	mov    esi,r15d
   bab6a:	c1 e8 06             	shr    eax,0x6
   bab6d:	83 e6 3f             	and    esi,0x3f
   bab70:	48 c1 e0 03          	shl    rax,0x3
   bab74:	4d 8d 94 06 80 00 00 00 	lea    r10,[r14+rax*1+0x80]
   bab7c:	49 8b 12             	mov    rdx,QWORD PTR [r10]
   bab7f:	4c 0f a3 fa          	bt     rdx,r15
   bab83:	0f 83 65 ff ff ff    	jae    baaee <lj_gc2_small_lease_try+0x14e>
   bab89:	4d 8d 9c 06 80 20 00 00 	lea    r11,[r14+rax*1+0x2080]
   bab91:	49 8b 13             	mov    rdx,QWORD PTR [r11]
   bab94:	48 0f a3 f2          	bt     rdx,rsi
   bab98:	0f 82 50 ff ff ff    	jb     baaee <lj_gc2_small_lease_try+0x14e>
   bab9e:	43 8d 54 2d 00       	lea    edx,[r13+r13*1+0x0]
   baba3:	89 d7                	mov    edi,edx
   baba5:	44 89 ea             	mov    edx,r13d
   baba8:	c1 ea 05             	shr    edx,0x5
   babab:	83 e7 3e             	and    edi,0x3e
   babae:	81 c2 90 00 00 00    	add    edx,0x90
   babb4:	89 7c 24 10          	mov    DWORD PTR [rsp+0x10],edi
   babb8:	89 f9                	mov    ecx,edi
   babba:	4d 8d 3c d6          	lea    r15,[r14+rdx*8]
   babbe:	49 8b 17             	mov    rdx,QWORD PTR [r15]
   babc1:	48 d3 ea             	shr    rdx,cl
   babc4:	f7 d2                	not    edx
   babc6:	80 e2 03             	and    dl,0x3
   babc9:	0f 84 1f ff ff ff    	je     baaee <lj_gc2_small_lease_try+0x14e>
   babcf:	45 85 e4             	test   r12d,r12d
   babd2:	74 24                	je     babf8 <lj_gc2_small_lease_try+0x258>
   babd4:	49 8b 94 06 80 24 00 00 	mov    rdx,QWORD PTR [r14+rax*1+0x2480]
   babdc:	48 0f a3 f2          	bt     rdx,rsi
   babe0:	0f 83 08 ff ff ff    	jae    baaee <lj_gc2_small_lease_try+0x14e>
   babe6:	49 8b 84 06 80 22 00 00 	mov    rax,QWORD PTR [r14+rax*1+0x2280]
   babee:	48 0f a3 f0          	bt     rax,rsi
   babf2:	0f 82 f6 fe ff ff    	jb     baaee <lj_gc2_small_lease_try+0x14e>
   babf8:	41 8d 95 00 04 00 00 	lea    edx,[r13+0x400]
   babff:	b8 00 10 00 00       	mov    eax,0x1000
   bac04:	89 74 24 0c          	mov    DWORD PTR [rsp+0xc],esi
   bac08:	41 8d 4d 01          	lea    ecx,[r13+0x1]
   bac0c:	39 c2                	cmp    edx,eax
   bac0e:	0f 47 d0             	cmova  edx,eax
   bac11:	eb 3b                	jmp    bac4e <lj_gc2_small_lease_try+0x2ae>
   bac13:	89 c8                	mov    eax,ecx
   bac15:	c1 e8 06             	shr    eax,0x6
   bac18:	89 44 24 18          	mov    DWORD PTR [rsp+0x18],eax
   bac1c:	8d 78 10             	lea    edi,[rax+0x10]
   bac1f:	49 8b 34 fe          	mov    rsi,QWORD PTR [r14+rdi*8]
   bac23:	48 8d 04 fd 00 00 00 00 	lea    rax,[rdi*8+0x0]
   bac2b:	48 83 cf ff          	or     rdi,0xffffffffffffffff
   bac2f:	48 d3 e7             	shl    rdi,cl
   bac32:	49 8b 84 06 00 02 00 00 	mov    rax,QWORD PTR [r14+rax*1+0x200]
   bac3a:	48 09 f0             	or     rax,rsi
   bac3d:	48 21 c7             	and    rdi,rax
   bac40:	75 2f                	jne    bac71 <lj_gc2_small_lease_try+0x2d1>
   bac42:	8b 44 24 18          	mov    eax,DWORD PTR [rsp+0x18]
   bac46:	83 c0 01             	add    eax,0x1
   bac49:	c1 e0 06             	shl    eax,0x6
   bac4c:	89 c1                	mov    ecx,eax
   bac4e:	39 d1                	cmp    ecx,edx
   bac50:	72 c1                	jb     bac13 <lj_gc2_small_lease_try+0x273>
   bac52:	8b 74 24 0c          	mov    esi,DWORD PTR [rsp+0xc]
   bac56:	eb 2e                	jmp    bac86 <lj_gc2_small_lease_try+0x2e6>
   bac58:	0f 1f 84 00 00 00 00 00 	nop    DWORD PTR [rax+rax*1+0x0]
   bac60:	48 8d 7c 24 40       	lea    rdi,[rsp+0x40]
   bac65:	31 f6                	xor    esi,esi
   bac67:	e8 f4 8f f7 ff       	call   33c60 <lj_arena_hugetab_reader_release>
   bac6c:	e9 98 fd ff ff       	jmp    baa09 <lj_gc2_small_lease_try+0x69>
   bac71:	89 c8                	mov    eax,ecx
   bac73:	f3 48 0f bc ff       	tzcnt  rdi,rdi
   bac78:	8b 74 24 0c          	mov    esi,DWORD PTR [rsp+0xc]
   bac7c:	83 e0 c0             	and    eax,0xffffffc0
   bac7f:	01 f8                	add    eax,edi
   bac81:	39 c2                	cmp    edx,eax
   bac83:	0f 47 d0             	cmova  edx,eax
   bac86:	89 d0                	mov    eax,edx
   bac88:	44 29 e8             	sub    eax,r13d
   bac8b:	48 c1 e0 04          	shl    rax,0x4
   bac8f:	48 39 e8             	cmp    rax,rbp
   bac92:	0f 82 56 fe ff ff    	jb     baaee <lj_gc2_small_lease_try+0x14e>
   bac98:	45 85 e4             	test   r12d,r12d
   bac9b:	74 17                	je     bacb4 <lj_gc2_small_lease_try+0x314>
   bac9d:	48 83 fd 09          	cmp    rbp,0x9
   baca1:	0f 86 47 fe ff ff    	jbe    baaee <lj_gc2_small_lease_try+0x14e>
   baca7:	0f b6 53 09          	movzx  edx,BYTE PTR [rbx+0x9]
   bacab:	44 39 e2             	cmp    edx,r12d
   bacae:	0f 85 3a fe ff ff    	jne    baaee <lj_gc2_small_lease_try+0x14e>
   bacb4:	49 8b 12             	mov    rdx,QWORD PTR [r10]
   bacb7:	48 0f a3 f2          	bt     rdx,rsi
   bacbb:	0f 83 2d fe ff ff    	jae    baaee <lj_gc2_small_lease_try+0x14e>
   bacc1:	49 8b 13             	mov    rdx,QWORD PTR [r11]
   bacc4:	48 0f a3 f2          	bt     rdx,rsi
   bacc8:	0f 82 20 fe ff ff    	jb     baaee <lj_gc2_small_lease_try+0x14e>
   bacce:	49 8b 17             	mov    rdx,QWORD PTR [r15]
   bacd1:	0f b6 4c 24 10       	movzx  ecx,BYTE PTR [rsp+0x10]
   bacd6:	48 d3 ea             	shr    rdx,cl
   bacd9:	f7 d2                	not    edx
   bacdb:	80 e2 03             	and    dl,0x3
   bacde:	0f 84 0a fe ff ff    	je     baaee <lj_gc2_small_lease_try+0x14e>
   bace4:	45 85 e4             	test   r12d,r12d
   bace7:	74 3d                	je     bad26 <lj_gc2_small_lease_try+0x386>
   bace9:	48 81 fb ff ff 00 00 	cmp    rbx,0xffff
   bacf0:	0f 86 f8 fd ff ff    	jbe    baaee <lj_gc2_small_lease_try+0x14e>
   bacf6:	44 89 ea             	mov    edx,r13d
   bacf9:	42 8d 0c ad 00 00 00 00 	lea    ecx,[r13*4+0x0]
   bad01:	c1 ea 04             	shr    edx,0x4
   bad04:	83 e1 3c             	and    ecx,0x3c
   bad07:	81 c2 10 02 00 00    	add    edx,0x210
   bad0d:	49 8b 14 d6          	mov    rdx,QWORD PTR [r14+rdx*8]
   bad11:	48 d3 ea             	shr    rdx,cl
   bad14:	b9 a6 01 00 00       	mov    ecx,0x1a6
   bad19:	83 e2 0f             	and    edx,0xf
   bad1c:	48 0f a3 d1          	bt     rcx,rdx
   bad20:	0f 83 c8 fd ff ff    	jae    baaee <lj_gc2_small_lease_try+0x14e>
   bad26:	48 8b 54 24 30       	mov    rdx,QWORD PTR [rsp+0x30]
   bad2b:	48 63 4c 24 38       	movsxd rcx,DWORD PTR [rsp+0x38]
   bad30:	49 89 10             	mov    QWORD PTR [r8],rdx
   bad33:	49 89 48 08          	mov    QWORD PTR [r8+0x8],rcx
   bad37:	81 f9 ce bc b8 ff    	cmp    ecx,0xffb8bcce
   bad3d:	75 14                	jne    bad53 <lj_gc2_small_lease_try+0x3b3>
   bad3f:	48 8b 54 24 50       	mov    rdx,QWORD PTR [rsp+0x50]
   bad44:	66 0f 6f 44 24 40    	movdqa xmm0,XMMWORD PTR [rsp+0x40]
   bad4a:	49 89 50 20          	mov    QWORD PTR [r8+0x20],rdx
   bad4e:	41 0f 11 40 10       	movups XMMWORD PTR [r8+0x10],xmm0
   bad53:	4d 85 c9             	test   r9,r9
   bad56:	74 03                	je     bad5b <lj_gc2_small_lease_try+0x3bb>
   bad58:	49 89 01             	mov    QWORD PTR [r9],rax
   bad5b:	b8 01 00 00 00       	mov    eax,0x1
   bad60:	e9 a6 fc ff ff       	jmp    baa0b <lj_gc2_small_lease_try+0x6b>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000bad70 <lj_gc2_storage_lease_try>:
   bad70:	41 57                	push   r15
   bad72:	41 56                	push   r14
   bad74:	49 89 d6             	mov    r14,rdx
   bad77:	41 55                	push   r13
   bad79:	49 89 fd             	mov    r13,rdi
   bad7c:	41 54                	push   r12
   bad7e:	49 89 f4             	mov    r12,rsi
   bad81:	55                   	push   rbp
   bad82:	4c 89 c5             	mov    rbp,r8
   bad85:	53                   	push   rbx
   bad86:	48 89 cb             	mov    rbx,rcx
   bad89:	48 83 ec 38          	sub    rsp,0x38
   bad8d:	66 81 fe 80 00       	cmp    si,0x80
   bad92:	75 6c                	jne    bae00 <lj_gc2_storage_lease_try+0x90>
   bad94:	48 85 c9             	test   rcx,rcx
   bad97:	74 4f                	je     bade8 <lj_gc2_storage_lease_try+0x78>
   bad99:	48 85 d2             	test   rdx,rdx
   bad9c:	66 0f ef c0          	pxor   xmm0,xmm0
   bada0:	48 c7 41 20 00 00 00 00 	mov    QWORD PTR [rcx+0x20],0x0
   bada8:	0f 94 c0             	sete   al
   badab:	48 85 ff             	test   rdi,rdi
   badae:	0f 11 01             	movups XMMWORD PTR [rcx],xmm0
   badb1:	0f 94 c2             	sete   dl
   badb4:	0f 11 41 10          	movups XMMWORD PTR [rcx+0x10],xmm0
   badb8:	09 d0                	or     eax,edx
   badba:	4d 85 c0             	test   r8,r8
   badbd:	74 07                	je     badc6 <lj_gc2_storage_lease_try+0x56>
   badbf:	49 c7 00 00 00 00 00 	mov    QWORD PTR [r8],0x0
   badc6:	84 c0                	test   al,al
   badc8:	75 09                	jne    badd3 <lj_gc2_storage_lease_try+0x63>
   badca:	4c 89 e0             	mov    rax,r12
   badcd:	48 c1 e8 2f          	shr    rax,0x2f
   badd1:	74 4d                	je     bae20 <lj_gc2_storage_lease_try+0xb0>
   badd3:	31 c0                	xor    eax,eax
   badd5:	48 83 c4 38          	add    rsp,0x38
   badd9:	5b                   	pop    rbx
   badda:	5d                   	pop    rbp
   baddb:	41 5c                	pop    r12
   baddd:	41 5d                	pop    r13
   baddf:	41 5e                	pop    r14
   bade1:	41 5f                	pop    r15
   bade3:	c3                   	ret
   bade4:	0f 1f 40 00          	nop    DWORD PTR [rax+0x0]
   bade8:	4d 85 c0             	test   r8,r8
   badeb:	74 e6                	je     badd3 <lj_gc2_storage_lease_try+0x63>
   baded:	49 c7 00 00 00 00 00 	mov    QWORD PTR [r8],0x0
   badf4:	eb dd                	jmp    badd3 <lj_gc2_storage_lease_try+0x63>
   badf6:	66 2e 0f 1f 84 00 00 00 00 00 	cs nop WORD PTR [rax+rax*1+0x0]
   bae00:	48 83 c4 38          	add    rsp,0x38
   bae04:	4d 89 c1             	mov    r9,r8
   bae07:	49 89 c8             	mov    r8,rcx
   bae0a:	31 c9                	xor    ecx,ecx
   bae0c:	5b                   	pop    rbx
   bae0d:	5d                   	pop    rbp
   bae0e:	41 5c                	pop    r12
   bae10:	41 5d                	pop    r13
   bae12:	41 5e                	pop    r14
   bae14:	41 5f                	pop    r15
   bae16:	e9 85 fb ff ff       	jmp    ba9a0 <lj_gc2_small_lease_try>
   bae1b:	0f 1f 44 00 00       	nop    DWORD PTR [rax+rax*1+0x0]
   bae20:	48 8d 74 24 14       	lea    rsi,[rsp+0x14]
   bae25:	ba 01 00 00 00       	mov    edx,0x1
   bae2a:	4c 89 ef             	mov    rdi,r13
   bae2d:	e8 ee c5 fe ff       	call   a7420 <gc2_registry_read_try_mode>
   bae32:	85 c0                	test   eax,eax
   bae34:	74 9d                	je     badd3 <lj_gc2_storage_lease_try+0x63>
   bae36:	48 8d 54 24 18       	lea    rdx,[rsp+0x18]
   bae3b:	31 c9                	xor    ecx,ecx
   bae3d:	4c 89 e6             	mov    rsi,r12
   bae40:	4c 89 ef             	mov    rdi,r13
   bae43:	e8 78 4d fe ff       	call   9fbc0 <gc2_huge_table_for_address>
   bae48:	44 8b 7c 24 14       	mov    r15d,DWORD PTR [rsp+0x14]
   bae4d:	48 85 c0             	test   rax,rax
   bae50:	74 07                	je     bae59 <lj_gc2_storage_lease_try+0xe9>
   bae52:	4c 39 64 24 18       	cmp    QWORD PTR [rsp+0x18],r12
   bae57:	74 37                	je     bae90 <lj_gc2_storage_lease_try+0x120>
   bae59:	45 85 ff             	test   r15d,r15d
   bae5c:	0f 84 71 ff ff ff    	je     badd3 <lj_gc2_storage_lease_try+0x63>
   bae62:	41 83 ff 01          	cmp    r15d,0x1
   bae66:	74 18                	je     bae80 <lj_gc2_storage_lease_try+0x110>
   bae68:	f0 41 83 ad 80 0e 00 00 01 	lock sub DWORD PTR [r13+0xe80],0x1
   bae71:	e9 5d ff ff ff       	jmp    badd3 <lj_gc2_storage_lease_try+0x63>
   bae76:	66 2e 0f 1f 84 00 00 00 00 00 	cs nop WORD PTR [rax+rax*1+0x0]
   bae80:	4c 89 ef             	mov    rdi,r13
   bae83:	e8 68 c7 fe ff       	call   a75f0 <lj_gc2_smr_read_leave>
   bae88:	e9 46 ff ff ff       	jmp    badd3 <lj_gc2_storage_lease_try+0x63>
   bae8d:	0f 1f 00             	nop    DWORD PTR [rax]
   bae90:	48 8d 4c 24 20       	lea    rcx,[rsp+0x20]
   bae95:	48 8d 53 10          	lea    rdx,[rbx+0x10]
   bae99:	4c 89 e6             	mov    rsi,r12
   bae9c:	48 89 c7             	mov    rdi,rax
   bae9f:	e8 5c 87 f7 ff       	call   33600 <lj_arena_hugetab_reader_acquire>
   baea4:	83 f8 01             	cmp    eax,0x1
   baea7:	75 b0                	jne    bae59 <lj_gc2_storage_lease_try+0xe9>
   baea9:	45 85 ff             	test   r15d,r15d
   baeac:	74 0f                	je     baebd <lj_gc2_storage_lease_try+0x14d>
   baeae:	41 83 ff 01          	cmp    r15d,0x1
   baeb2:	74 49                	je     baefd <lj_gc2_storage_lease_try+0x18d>
   baeb4:	f0 41 83 ad 80 0e 00 00 01 	lock sub DWORD PTR [r13+0xe80],0x1
   baebd:	48 c7 43 08 ce bc b8 ff 	mov    QWORD PTR [rbx+0x8],0xffffffffffb8bcce
   baec5:	f6 44 24 28 02       	test   BYTE PTR [rsp+0x28],0x2
   baeca:	75 24                	jne    baef0 <lj_gc2_storage_lease_try+0x180>
   baecc:	48 8b 54 24 20       	mov    rdx,QWORD PTR [rsp+0x20]
   baed1:	4c 39 f2             	cmp    rdx,r14
   baed4:	72 1a                	jb     baef0 <lj_gc2_storage_lease_try+0x180>
   baed6:	48 85 ed             	test   rbp,rbp
   baed9:	0f 84 f6 fe ff ff    	je     badd5 <lj_gc2_storage_lease_try+0x65>
   baedf:	48 89 55 00          	mov    QWORD PTR [rbp+0x0],rdx
   baee3:	e9 ed fe ff ff       	jmp    badd5 <lj_gc2_storage_lease_try+0x65>
   baee8:	0f 1f 84 00 00 00 00 00 	nop    DWORD PTR [rax+rax*1+0x0]
   baef0:	48 89 df             	mov    rdi,rbx
   baef3:	e8 b8 75 fe ff       	call   a24b0 <lj_gc2_lease_release.part.0>
   baef8:	e9 d6 fe ff ff       	jmp    badd3 <lj_gc2_storage_lease_try+0x63>
   baefd:	4c 89 ef             	mov    rdi,r13
   baf00:	89 44 24 0c          	mov    DWORD PTR [rsp+0xc],eax
   baf04:	e8 e7 c6 fe ff       	call   a75f0 <lj_gc2_smr_read_leave>
   baf09:	8b 44 24 0c          	mov    eax,DWORD PTR [rsp+0xc]
   baf0d:	eb ae                	jmp    baebd <lj_gc2_storage_lease_try+0x14d>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000ba910 <lj_gc2_lease_release>:
   ba910:	48 85 ff             	test   rdi,rdi
   ba913:	74 5b                	je     ba970 <lj_gc2_lease_release+0x60>
   ba915:	48 83 ec 38          	sub    rsp,0x38
   ba919:	f3 0f 6f 47 10       	movdqu xmm0,XMMWORD PTR [rdi+0x10]
   ba91e:	48 8b 17             	mov    rdx,QWORD PTR [rdi]
   ba921:	48 8b 47 08          	mov    rax,QWORD PTR [rdi+0x8]
   ba925:	48 8b 4f 20          	mov    rcx,QWORD PTR [rdi+0x20]
   ba929:	48 c7 47 20 00 00 00 00 	mov    QWORD PTR [rdi+0x20],0x0
   ba931:	0f 29 44 24 10       	movaps XMMWORD PTR [rsp+0x10],xmm0
   ba936:	66 0f ef c0          	pxor   xmm0,xmm0
   ba93a:	48 89 14 24          	mov    QWORD PTR [rsp],rdx
   ba93e:	89 44 24 08          	mov    DWORD PTR [rsp+0x8],eax
   ba942:	48 89 4c 24 20       	mov    QWORD PTR [rsp+0x20],rcx
   ba947:	0f 11 07             	movups XMMWORD PTR [rdi],xmm0
   ba94a:	0f 11 47 10          	movups XMMWORD PTR [rdi+0x10],xmm0
   ba94e:	3d ce bc b8 ff       	cmp    eax,0xffb8bcce
   ba953:	74 33                	je     ba988 <lj_gc2_lease_release+0x78>
   ba955:	48 85 d2             	test   rdx,rdx
   ba958:	74 08                	je     ba962 <lj_gc2_lease_release+0x52>
   ba95a:	83 e8 01             	sub    eax,0x1
   ba95d:	83 f8 02             	cmp    eax,0x2
   ba960:	76 16                	jbe    ba978 <lj_gc2_lease_release+0x68>
   ba962:	48 83 c4 38          	add    rsp,0x38
   ba966:	c3                   	ret
   ba967:	66 0f 1f 84 00 00 00 00 00 	nop    WORD PTR [rax+rax*1+0x0]
   ba970:	c3                   	ret
   ba971:	0f 1f 80 00 00 00 00 	nop    DWORD PTR [rax+0x0]
   ba978:	48 89 d7             	mov    rdi,rdx
   ba97b:	e8 80 30 f7 ff       	call   2da00 <lj_arena_rescue_leave>
   ba980:	48 83 c4 38          	add    rsp,0x38
   ba984:	c3                   	ret
   ba985:	0f 1f 00             	nop    DWORD PTR [rax]
   ba988:	48 8d 7c 24 10       	lea    rdi,[rsp+0x10]
   ba98d:	31 f6                	xor    esi,esi
   ba98f:	e8 cc 92 f7 ff       	call   33c60 <lj_arena_hugetab_reader_release>
   ba994:	48 83 c4 38          	add    rsp,0x38
   ba998:	c3                   	ret

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

0000000000022c30 <lj_tg_root_anchor_push>:
   22c30:	41 57                	push   r15
   22c32:	41 56                	push   r14
   22c34:	41 55                	push   r13
   22c36:	41 54                	push   r12
   22c38:	55                   	push   rbp
   22c39:	53                   	push   rbx
   22c3a:	48 83 ec 18          	sub    rsp,0x18
   22c3e:	48 89 54 24 08       	mov    QWORD PTR [rsp+0x8],rdx
   22c43:	48 85 f6             	test   rsi,rsi
   22c46:	0f 84 cf 00 00 00    	je     22d1b <lj_tg_root_anchor_push+0xeb>
   22c4c:	44 8b a6 a0 4d 00 00 	mov    r12d,DWORD PTR [rsi+0x4da0]
   22c53:	48 89 fb             	mov    rbx,rdi
   22c56:	49 89 f7             	mov    r15,rsi
   22c59:	41 81 fc 3f 42 0f 00 	cmp    r12d,0xf423f
   22c60:	0f 87 c6 00 00 00    	ja     22d2c <lj_tg_root_anchor_push+0xfc>
   22c66:	44 89 e2             	mov    edx,r12d
   22c69:	48 89 cd             	mov    rbp,rcx
   22c6c:	48 8d 86 18 4d 00 00 	lea    rax,[rsi+0x4d18]
   22c73:	c1 ea 04             	shr    edx,0x4
   22c76:	44 8d 6a ff          	lea    r13d,[rdx-0x1]
   22c7a:	75 0a                	jne    22c86 <lj_tg_root_anchor_push+0x56>
   22c7c:	eb 63                	jmp    22ce1 <lj_tg_root_anchor_push+0xb1>
   22c7e:	66 90                	xchg   ax,ax
   22c80:	41 83 ed 01          	sub    r13d,0x1
   22c84:	72 5b                	jb     22ce1 <lj_tg_root_anchor_push+0xb1>
   22c86:	49 89 c6             	mov    r14,rax
   22c89:	48 8b 00             	mov    rax,QWORD PTR [rax]
   22c8c:	48 85 c0             	test   rax,rax
   22c8f:	75 ef                	jne    22c80 <lj_tg_root_anchor_push+0x50>
   22c91:	b9 88 00 00 00       	mov    ecx,0x88
   22c96:	31 d2                	xor    edx,edx
   22c98:	31 f6                	xor    esi,esi
   22c9a:	48 89 df             	mov    rdi,rbx
   22c9d:	e8 2e 5c 07 00       	call   988d0 <lj_mem_realloc>
   22ca2:	48 8d 50 08          	lea    rdx,[rax+0x8]
   22ca6:	48 8d 88 88 00 00 00 	lea    rcx,[rax+0x88]
   22cad:	48 c7 00 00 00 00 00 	mov    QWORD PTR [rax],0x0
   22cb4:	66 66 2e 0f 1f 84 00 00 00 00 00 	data16 cs nop WORD PTR [rax+rax*1+0x0]
   22cbf:	90                   	nop
   22cc0:	48 c7 02 ff ff ff ff 	mov    QWORD PTR [rdx],0xffffffffffffffff
   22cc7:	48 83 c2 10          	add    rdx,0x10
   22ccb:	48 c7 42 f8 ff ff ff ff 	mov    QWORD PTR [rdx-0x8],0xffffffffffffffff
   22cd3:	48 39 ca             	cmp    rdx,rcx
   22cd6:	75 e8                	jne    22cc0 <lj_tg_root_anchor_push+0x90>
   22cd8:	49 89 06             	mov    QWORD PTR [r14],rax
   22cdb:	41 83 ed 01          	sub    r13d,0x1
   22cdf:	73 a5                	jae    22c86 <lj_tg_root_anchor_push+0x56>
   22ce1:	48 8b 5c 24 08       	mov    rbx,QWORD PTR [rsp+0x8]
   22ce6:	44 89 e2             	mov    edx,r12d
   22ce9:	83 e2 0f             	and    edx,0xf
   22cec:	48 8d 44 d0 08       	lea    rax,[rax+rdx*8+0x8]
   22cf1:	48 8b 13             	mov    rdx,QWORD PTR [rbx]
   22cf4:	48 89 10             	mov    QWORD PTR [rax],rdx
   22cf7:	41 8d 54 24 01       	lea    edx,[r12+0x1]
   22cfc:	41 89 97 a0 4d 00 00 	mov    DWORD PTR [r15+0x4da0],edx
   22d03:	48 85 ed             	test   rbp,rbp
   22d06:	74 04                	je     22d0c <lj_tg_root_anchor_push+0xdc>
   22d08:	44 89 65 00          	mov    DWORD PTR [rbp+0x0],r12d
   22d0c:	48 83 c4 18          	add    rsp,0x18
   22d10:	5b                   	pop    rbx
   22d11:	5d                   	pop    rbp
   22d12:	41 5c                	pop    r12
   22d14:	41 5d                	pop    r13
   22d16:	41 5e                	pop    r14
   22d18:	41 5f                	pop    r15
   22d1a:	c3                   	ret
   22d1b:	48 83 c4 18          	add    rsp,0x18
   22d1f:	31 c0                	xor    eax,eax
   22d21:	5b                   	pop    rbx
   22d22:	5d                   	pop    rbp
   22d23:	41 5c                	pop    r12
   22d25:	41 5d                	pop    r13
   22d27:	41 5e                	pop    r14
   22d29:	41 5f                	pop    r15
   22d2b:	c3                   	ret
   22d2c:	e8 af bd fe ff       	call   eae0 <lj_err_mem>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/current-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

0000000000022d40 <lj_tg_root_anchor_pop>:
   22d40:	89 f2                	mov    edx,esi
   22d42:	48 8d 87 18 4d 00 00 	lea    rax,[rdi+0x4d18]
   22d49:	c1 ea 04             	shr    edx,0x4
   22d4c:	48 85 ff             	test   rdi,rdi
   22d4f:	75 17                	jne    22d68 <lj_tg_root_anchor_pop+0x28>
   22d51:	c3                   	ret
   22d52:	66 66 2e 0f 1f 84 00 00 00 00 00 	data16 cs nop WORD PTR [rax+rax*1+0x0]
   22d5d:	0f 1f 00             	nop    DWORD PTR [rax]
   22d60:	48 8b 00             	mov    rax,QWORD PTR [rax]
   22d63:	48 85 c0             	test   rax,rax
   22d66:	74 23                	je     22d8b <lj_tg_root_anchor_pop+0x4b>
   22d68:	83 ea 01             	sub    edx,0x1
   22d6b:	73 f3                	jae    22d60 <lj_tg_root_anchor_pop+0x20>
   22d6d:	89 f2                	mov    edx,esi
   22d6f:	83 e2 0f             	and    edx,0xf
   22d72:	48 83 c2 01          	add    rdx,0x1
   22d76:	48 c7 04 d0 ff ff ff ff 	mov    QWORD PTR [rax+rdx*8],0xffffffffffffffff
   22d7e:	8d 56 01             	lea    edx,[rsi+0x1]
   22d81:	8b 87 a0 4d 00 00    	mov    eax,DWORD PTR [rdi+0x4da0]
   22d87:	39 c2                	cmp    edx,eax
   22d89:	74 05                	je     22d90 <lj_tg_root_anchor_pop+0x50>
   22d8b:	c3                   	ret
   22d8c:	0f 1f 40 00          	nop    DWORD PTR [rax+0x0]
   22d90:	89 b7 a0 4d 00 00    	mov    DWORD PTR [rdi+0x4da0],esi
   22d96:	c3                   	ret

Disassembly of section .fini:
