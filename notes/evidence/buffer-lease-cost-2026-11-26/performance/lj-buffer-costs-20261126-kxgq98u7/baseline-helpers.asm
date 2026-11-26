
/tmp/lj-buffer-costs-20261126-kxgq98u7/baseline-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cb9d0 <lj_bufx_putstr_forjit>:
   cb9d0:	53                   	push   rbx
   cb9d1:	48 89 fb             	mov    rbx,rdi
   cb9d4:	48 89 f7             	mov    rdi,rsi
   cb9d7:	48 89 d6             	mov    rsi,rdx
   cb9da:	48 8b 47 18          	mov    rax,QWORD PTR [rdi+0x18]
   cb9de:	83 e0 07             	and    eax,0x7
   cb9e1:	48 09 d8             	or     rax,rbx
   cb9e4:	48 89 47 18          	mov    QWORD PTR [rdi+0x18],rax
   cb9e8:	e8 73 ff ff ff       	call   cb960 <lj_buf_putstr>
   cb9ed:	48 8b 7b 10          	mov    rdi,QWORD PTR [rbx+0x10]
   cb9f1:	48 8b 57 48          	mov    rdx,QWORD PTR [rdi+0x48]
   cb9f5:	48 8b 47 50          	mov    rax,QWORD PTR [rdi+0x50]
   cb9f9:	48 39 c2             	cmp    rdx,rax
   cb9fc:	72 12                	jb     cba10 <lj_bufx_putstr_forjit+0x40>
   cb9fe:	48 89 df             	mov    rdi,rbx
   cba01:	5b                   	pop    rbx
   cba02:	e9 e9 c4 fc ff       	jmp    97ef0 <lj_gc_step_top>
   cba07:	66 0f 1f 84 00 00 00 00 00 	nop    WORD PTR [rax+rax*1+0x0]
   cba10:	48 8b 87 78 10 00 00 	mov    rax,QWORD PTR [rdi+0x1078]
   cba17:	48 8b 97 90 10 00 00 	mov    rdx,QWORD PTR [rdi+0x1090]
   cba1e:	48 39 c2             	cmp    rdx,rax
   cba21:	72 db                	jb     cb9fe <lj_bufx_putstr_forjit+0x2e>
   cba23:	8b 87 a0 0c 00 00    	mov    eax,DWORD PTR [rdi+0xca0]
   cba29:	85 c0                	test   eax,eax
   cba2b:	75 03                	jne    cba30 <lj_bufx_putstr_forjit+0x60>
   cba2d:	5b                   	pop    rbx
   cba2e:	c3                   	ret
   cba2f:	90                   	nop
   cba30:	e8 7b 88 fc ff       	call   942b0 <lj_gc_pending_auto_request>
   cba35:	85 c0                	test   eax,eax
   cba37:	75 c5                	jne    cb9fe <lj_bufx_putstr_forjit+0x2e>
   cba39:	eb f2                	jmp    cba2d <lj_bufx_putstr_forjit+0x5d>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/baseline-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cb960 <lj_buf_putstr>:
   cb960:	41 54                	push   r12
   cb962:	55                   	push   rbp
   cb963:	48 89 f5             	mov    rbp,rsi
   cb966:	53                   	push   rbx
   cb967:	44 8b 66 14          	mov    r12d,DWORD PTR [rsi+0x14]
   cb96b:	48 89 fb             	mov    rbx,rdi
   cb96e:	48 8b 4f 10          	mov    rcx,QWORD PTR [rdi+0x10]
   cb972:	48 8b 47 08          	mov    rax,QWORD PTR [rdi+0x8]
   cb976:	48 8b 17             	mov    rdx,QWORD PTR [rdi]
   cb979:	48 39 d0             	cmp    rax,rdx
   cb97c:	48 89 d6             	mov    rsi,rdx
   cb97f:	48 0f 46 f0          	cmovbe rsi,rax
   cb983:	48 39 ce             	cmp    rsi,rcx
   cb986:	72 05                	jb     cb98d <lj_buf_putstr+0x2d>
   cb988:	48 39 d0             	cmp    rax,rdx
   cb98b:	73 2b                	jae    cb9b8 <lj_buf_putstr+0x58>
   cb98d:	31 c0                	xor    eax,eax
   cb98f:	44 39 e0             	cmp    eax,r12d
   cb992:	72 2b                	jb     cb9bf <lj_buf_putstr+0x5f>
   cb994:	48 8b 3b             	mov    rdi,QWORD PTR [rbx]
   cb997:	48 8d 75 30          	lea    rsi,[rbp+0x30]
   cb99b:	4c 89 e2             	mov    rdx,r12
   cb99e:	e8 4d dd f3 ff       	call   96f0 <memcpy@plt>
   cb9a3:	4c 01 e0             	add    rax,r12
   cb9a6:	48 89 03             	mov    QWORD PTR [rbx],rax
   cb9a9:	48 89 d8             	mov    rax,rbx
   cb9ac:	5b                   	pop    rbx
   cb9ad:	5d                   	pop    rbp
   cb9ae:	41 5c                	pop    r12
   cb9b0:	c3                   	ret
   cb9b1:	0f 1f 80 00 00 00 00 	nop    DWORD PTR [rax+0x0]
   cb9b8:	29 d0                	sub    eax,edx
   cb9ba:	44 39 e0             	cmp    eax,r12d
   cb9bd:	73 d5                	jae    cb994 <lj_buf_putstr+0x34>
   cb9bf:	48 89 df             	mov    rdi,rbx
   cb9c2:	44 89 e6             	mov    esi,r12d
   cb9c5:	e8 46 f1 ff ff       	call   cab10 <lj_buf_more2>
   cb9ca:	48 89 c7             	mov    rdi,rax
   cb9cd:	eb c8                	jmp    cb997 <lj_buf_putstr+0x37>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/baseline-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cb780 <lj_bufx_reset_forjit>:
   cb780:	53                   	push   rbx
   cb781:	48 8b 47 18          	mov    rax,QWORD PTR [rdi+0x18]
   cb785:	a8 02                	test   al,0x2
   cb787:	75 37                	jne    cb7c0 <lj_bufx_reset_forjit+0x40>
   cb789:	4c 8b 47 10          	mov    r8,QWORD PTR [rdi+0x10]
   cb78d:	31 c0                	xor    eax,eax
   cb78f:	48 8d 77 20          	lea    rsi,[rdi+0x20]
   cb793:	48 89 c2             	mov    rdx,rax
   cb796:	48 89 c3             	mov    rbx,rax
   cb799:	48 89 c1             	mov    rcx,rax
   cb79c:	4c 89 07             	mov    QWORD PTR [rdi],r8
   cb79f:	f0 48 0f c7 4f 20    	lock cmpxchg16b OWORD PTR [rdi+0x20]
   cb7a5:	66 66 2e 0f 1f 84 00 00 00 00 00 	data16 cs nop WORD PTR [rax+rax*1+0x0]
   cb7b0:	48 8d 4a 01          	lea    rcx,[rdx+0x1]
   cb7b4:	4c 89 c3             	mov    rbx,r8
   cb7b7:	f0 48 0f c7 0e       	lock cmpxchg16b OWORD PTR [rsi]
   cb7bc:	75 f2                	jne    cb7b0 <lj_bufx_reset_forjit+0x30>
   cb7be:	5b                   	pop    rbx
   cb7bf:	c3                   	ret
   cb7c0:	48 8b 47 18          	mov    rax,QWORD PTR [rdi+0x18]
   cb7c4:	48 83 e0 fd          	and    rax,0xfffffffffffffffd
   cb7c8:	48 89 47 18          	mov    QWORD PTR [rdi+0x18],rax
   cb7cc:	48 c7 47 30 00 00 00 00 	mov    QWORD PTR [rdi+0x30],0x0
   cb7d4:	48 c7 47 10 00 00 00 00 	mov    QWORD PTR [rdi+0x10],0x0
   cb7dc:	48 c7 47 08 00 00 00 00 	mov    QWORD PTR [rdi+0x8],0x0
   cb7e4:	eb a3                	jmp    cb789 <lj_bufx_reset_forjit+0x9>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/baseline-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cb650 <lj_bufx_get>:
   cb650:	b9 01 00 00 00       	mov    ecx,0x1
   cb655:	e9 76 f8 ff ff       	jmp    caed0 <bufx_string>

Disassembly of section .fini:

/tmp/lj-buffer-costs-20261126-kxgq98u7/baseline-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000ba920 <lj_gc2_small_lease_try>:
   ba920:	4d 85 c0             	test   r8,r8
   ba923:	74 7b                	je     ba9a0 <lj_gc2_small_lease_try+0x80>
   ba925:	41 57                	push   r15
   ba927:	66 0f ef c0          	pxor   xmm0,xmm0
   ba92b:	48 89 f8             	mov    rax,rdi
   ba92e:	41 56                	push   r14
   ba930:	41 55                	push   r13
   ba932:	41 54                	push   r12
   ba934:	41 89 cc             	mov    r12d,ecx
   ba937:	55                   	push   rbp
   ba938:	48 89 d5             	mov    rbp,rdx
   ba93b:	53                   	push   rbx
   ba93c:	48 89 f3             	mov    rbx,rsi
   ba93f:	48 83 ec 68          	sub    rsp,0x68
   ba943:	49 c7 40 20 00 00 00 00 	mov    QWORD PTR [r8+0x20],0x0
   ba94b:	41 0f 11 00          	movups XMMWORD PTR [r8],xmm0
   ba94f:	41 0f 11 40 10       	movups XMMWORD PTR [r8+0x10],xmm0
   ba954:	4d 85 c9             	test   r9,r9
   ba957:	74 07                	je     ba960 <lj_gc2_small_lease_try+0x40>
   ba959:	49 c7 01 00 00 00 00 	mov    QWORD PTR [r9],0x0
   ba960:	48 85 c0             	test   rax,rax
   ba963:	74 24                	je     ba989 <lj_gc2_small_lease_try+0x69>
   ba965:	48 85 db             	test   rbx,rbx
   ba968:	74 1f                	je     ba989 <lj_gc2_small_lease_try+0x69>
   ba96a:	48 8d 55 ff          	lea    rdx,[rbp-0x1]
   ba96e:	48 81 fa ff 3f 00 00 	cmp    rdx,0x3fff
   ba975:	77 12                	ja     ba989 <lj_gc2_small_lease_try+0x69>
   ba977:	48 89 da             	mov    rdx,rbx
   ba97a:	48 89 d9             	mov    rcx,rbx
   ba97d:	48 c1 ea 2f          	shr    rdx,0x2f
   ba981:	83 e1 0f             	and    ecx,0xf
   ba984:	48 09 ca             	or     rdx,rcx
   ba987:	74 1f                	je     ba9a8 <lj_gc2_small_lease_try+0x88>
   ba989:	31 c0                	xor    eax,eax
   ba98b:	48 83 c4 68          	add    rsp,0x68
   ba98f:	5b                   	pop    rbx
   ba990:	5d                   	pop    rbp
   ba991:	41 5c                	pop    r12
   ba993:	41 5d                	pop    r13
   ba995:	41 5e                	pop    r14
   ba997:	41 5f                	pop    r15
   ba999:	c3                   	ret
   ba99a:	66 0f 1f 44 00 00    	nop    WORD PTR [rax+rax*1+0x0]
   ba9a0:	31 c0                	xor    eax,eax
   ba9a2:	c3                   	ret
   ba9a3:	0f 1f 44 00 00       	nop    DWORD PTR [rax+rax*1+0x0]
   ba9a8:	8b 50 10             	mov    edx,DWORD PTR [rax+0x10]
   ba9ab:	85 d2                	test   edx,edx
   ba9ad:	74 da                	je     ba989 <lj_gc2_small_lease_try+0x69>
   ba9af:	45 85 e4             	test   r12d,r12d
   ba9b2:	0f 95 c1             	setne  cl
   ba9b5:	41 83 fc 0b          	cmp    r12d,0xb
   ba9b9:	74 0e                	je     ba9c9 <lj_gc2_small_lease_try+0xa9>
   ba9bb:	84 c9                	test   cl,cl
   ba9bd:	74 0a                	je     ba9c9 <lj_gc2_small_lease_try+0xa9>
   ba9bf:	41 8d 54 24 fc       	lea    edx,[r12-0x4]
   ba9c4:	83 fa 01             	cmp    edx,0x1
   ba9c7:	77 c0                	ja     ba989 <lj_gc2_small_lease_try+0x69>
   ba9c9:	4c 89 4c 24 18       	mov    QWORD PTR [rsp+0x18],r9
   ba9ce:	4c 89 44 24 10       	mov    QWORD PTR [rsp+0x10],r8
   ba9d3:	88 4c 24 0c          	mov    BYTE PTR [rsp+0xc],cl
   ba9d7:	48 8b b8 88 0f 00 00 	mov    rdi,QWORD PTR [rax+0xf88]
   ba9de:	48 85 ff             	test   rdi,rdi
   ba9e1:	74 a6                	je     ba989 <lj_gc2_small_lease_try+0x69>
   ba9e3:	49 89 df             	mov    r15,rbx
   ba9e6:	49 c1 ef 04          	shr    r15,0x4
   ba9ea:	45 89 fd             	mov    r13d,r15d
   ba9ed:	41 81 e5 ff 0f 00 00 	and    r13d,0xfff
   ba9f4:	41 8d 85 98 fd ff ff 	lea    eax,[r13-0x268]
   ba9fb:	3d 97 0d 00 00       	cmp    eax,0xd97
   baa00:	77 87                	ja     ba989 <lj_gc2_small_lease_try+0x69>
   baa02:	49 89 de             	mov    r14,rbx
   baa05:	66 0f ef c0          	pxor   xmm0,xmm0
   baa09:	48 8d 54 24 20       	lea    rdx,[rsp+0x20]
   baa0e:	48 c7 44 24 30 00 00 00 00 	mov    QWORD PTR [rsp+0x30],0x0
   baa17:	66 45 31 f6          	xor    r14w,r14w
   baa1b:	c7 44 24 38 00 00 00 00 	mov    DWORD PTR [rsp+0x38],0x0
   baa23:	4c 89 f6             	mov    rsi,r14
   baa26:	0f 29 44 24 40       	movaps XMMWORD PTR [rsp+0x40],xmm0
   baa2b:	48 c7 44 24 50 00 00 00 00 	mov    QWORD PTR [rsp+0x50],0x0
   baa34:	e8 07 e0 f7 ff       	call   38a40 <lj_arena_hugetab_observe_enter>
   baa39:	89 44 24 38          	mov    DWORD PTR [rsp+0x38],eax
   baa3d:	85 c0                	test   eax,eax
   baa3f:	0f 84 44 ff ff ff    	je     ba989 <lj_gc2_small_lease_try+0x69>
   baa45:	4c 89 74 24 30       	mov    QWORD PTR [rsp+0x30],r14
   baa4a:	f0 48 83 0c 24 00    	lock or QWORD PTR [rsp],0x0
   baa50:	48 81 7c 24 20 00 00 01 00 	cmp    QWORD PTR [rsp+0x20],0x10000
   baa59:	0f b6 4c 24 0c       	movzx  ecx,BYTE PTR [rsp+0xc]
   baa5e:	4c 8b 44 24 10       	mov    r8,QWORD PTR [rsp+0x10]
   baa63:	4c 8b 4c 24 18       	mov    r9,QWORD PTR [rsp+0x18]
   baa68:	74 33                	je     baa9d <lj_gc2_small_lease_try+0x17d>
   baa6a:	8b 44 24 38          	mov    eax,DWORD PTR [rsp+0x38]
   baa6e:	3d ce bc b8 ff       	cmp    eax,0xffb8bcce
   baa73:	0f 84 5f 01 00 00    	je     babd8 <lj_gc2_small_lease_try+0x2b8>
   baa79:	48 8b 7c 24 30       	mov    rdi,QWORD PTR [rsp+0x30]
   baa7e:	48 85 ff             	test   rdi,rdi
   baa81:	0f 84 02 ff ff ff    	je     ba989 <lj_gc2_small_lease_try+0x69>
   baa87:	83 e8 01             	sub    eax,0x1
   baa8a:	83 f8 02             	cmp    eax,0x2
   baa8d:	0f 87 f6 fe ff ff    	ja     ba989 <lj_gc2_small_lease_try+0x69>
   baa93:	e8 68 2f f7 ff       	call   2da00 <lj_arena_rescue_leave>
   baa98:	e9 ec fe ff ff       	jmp    ba989 <lj_gc2_small_lease_try+0x69>
   baa9d:	41 8b 06             	mov    eax,DWORD PTR [r14]
   baaa0:	83 e0 01             	and    eax,0x1
   baaa3:	38 c1                	cmp    cl,al
   baaa5:	75 c3                	jne    baa6a <lj_gc2_small_lease_try+0x14a>
   baaa7:	45 85 e4             	test   r12d,r12d
   baaaa:	74 34                	je     baae0 <lj_gc2_small_lease_try+0x1c0>
   baaac:	48 81 fb ff ff 00 00 	cmp    rbx,0xffff
   baab3:	76 b5                	jbe    baa6a <lj_gc2_small_lease_try+0x14a>
   baab5:	44 89 e8             	mov    eax,r13d
   baab8:	42 8d 0c ad 00 00 00 00 	lea    ecx,[r13*4+0x0]
   baac0:	ba a6 01 00 00       	mov    edx,0x1a6
   baac5:	c1 e8 04             	shr    eax,0x4
   baac8:	83 e1 3c             	and    ecx,0x3c
   baacb:	05 10 02 00 00       	add    eax,0x210
   baad0:	49 8b 04 c6          	mov    rax,QWORD PTR [r14+rax*8]
   baad4:	48 d3 e8             	shr    rax,cl
   baad7:	83 e0 0f             	and    eax,0xf
   baada:	48 0f a3 c2          	bt     rdx,rax
   baade:	73 8a                	jae    baa6a <lj_gc2_small_lease_try+0x14a>
   baae0:	44 89 e8             	mov    eax,r13d
   baae3:	44 89 fe             	mov    esi,r15d
   baae6:	c1 e8 06             	shr    eax,0x6
   baae9:	83 e6 3f             	and    esi,0x3f
   baaec:	48 c1 e0 03          	shl    rax,0x3
   baaf0:	4d 8d 94 06 80 00 00 00 	lea    r10,[r14+rax*1+0x80]
   baaf8:	49 8b 12             	mov    rdx,QWORD PTR [r10]
   baafb:	4c 0f a3 fa          	bt     rdx,r15
   baaff:	0f 83 65 ff ff ff    	jae    baa6a <lj_gc2_small_lease_try+0x14a>
   bab05:	4d 8d 9c 06 80 20 00 00 	lea    r11,[r14+rax*1+0x2080]
   bab0d:	49 8b 13             	mov    rdx,QWORD PTR [r11]
   bab10:	48 0f a3 f2          	bt     rdx,rsi
   bab14:	0f 82 50 ff ff ff    	jb     baa6a <lj_gc2_small_lease_try+0x14a>
   bab1a:	43 8d 54 2d 00       	lea    edx,[r13+r13*1+0x0]
   bab1f:	89 d7                	mov    edi,edx
   bab21:	44 89 ea             	mov    edx,r13d
   bab24:	c1 ea 05             	shr    edx,0x5
   bab27:	83 e7 3e             	and    edi,0x3e
   bab2a:	81 c2 90 00 00 00    	add    edx,0x90
   bab30:	89 7c 24 10          	mov    DWORD PTR [rsp+0x10],edi
   bab34:	89 f9                	mov    ecx,edi
   bab36:	4d 8d 3c d6          	lea    r15,[r14+rdx*8]
   bab3a:	49 8b 17             	mov    rdx,QWORD PTR [r15]
   bab3d:	48 d3 ea             	shr    rdx,cl
   bab40:	f7 d2                	not    edx
   bab42:	80 e2 03             	and    dl,0x3
   bab45:	0f 84 1f ff ff ff    	je     baa6a <lj_gc2_small_lease_try+0x14a>
   bab4b:	45 85 e4             	test   r12d,r12d
   bab4e:	74 24                	je     bab74 <lj_gc2_small_lease_try+0x254>
   bab50:	49 8b 94 06 80 24 00 00 	mov    rdx,QWORD PTR [r14+rax*1+0x2480]
   bab58:	48 0f a3 f2          	bt     rdx,rsi
   bab5c:	0f 83 08 ff ff ff    	jae    baa6a <lj_gc2_small_lease_try+0x14a>
   bab62:	49 8b 84 06 80 22 00 00 	mov    rax,QWORD PTR [r14+rax*1+0x2280]
   bab6a:	48 0f a3 f0          	bt     rax,rsi
   bab6e:	0f 82 f6 fe ff ff    	jb     baa6a <lj_gc2_small_lease_try+0x14a>
   bab74:	41 8d 95 00 04 00 00 	lea    edx,[r13+0x400]
   bab7b:	b8 00 10 00 00       	mov    eax,0x1000
   bab80:	89 74 24 0c          	mov    DWORD PTR [rsp+0xc],esi
   bab84:	41 8d 4d 01          	lea    ecx,[r13+0x1]
   bab88:	39 c2                	cmp    edx,eax
   bab8a:	0f 47 d0             	cmova  edx,eax
   bab8d:	eb 3b                	jmp    babca <lj_gc2_small_lease_try+0x2aa>
   bab8f:	89 c8                	mov    eax,ecx
   bab91:	c1 e8 06             	shr    eax,0x6
   bab94:	89 44 24 18          	mov    DWORD PTR [rsp+0x18],eax
   bab98:	8d 78 10             	lea    edi,[rax+0x10]
   bab9b:	49 8b 34 fe          	mov    rsi,QWORD PTR [r14+rdi*8]
   bab9f:	48 8d 04 fd 00 00 00 00 	lea    rax,[rdi*8+0x0]
   baba7:	48 83 cf ff          	or     rdi,0xffffffffffffffff
   babab:	48 d3 e7             	shl    rdi,cl
   babae:	49 8b 84 06 00 02 00 00 	mov    rax,QWORD PTR [r14+rax*1+0x200]
   babb6:	48 09 f0             	or     rax,rsi
   babb9:	48 21 c7             	and    rdi,rax
   babbc:	75 2b                	jne    babe9 <lj_gc2_small_lease_try+0x2c9>
   babbe:	8b 44 24 18          	mov    eax,DWORD PTR [rsp+0x18]
   babc2:	83 c0 01             	add    eax,0x1
   babc5:	c1 e0 06             	shl    eax,0x6
   babc8:	89 c1                	mov    ecx,eax
   babca:	39 d1                	cmp    ecx,edx
   babcc:	72 c1                	jb     bab8f <lj_gc2_small_lease_try+0x26f>
   babce:	8b 74 24 0c          	mov    esi,DWORD PTR [rsp+0xc]
   babd2:	eb 2a                	jmp    babfe <lj_gc2_small_lease_try+0x2de>
   babd4:	0f 1f 40 00          	nop    DWORD PTR [rax+0x0]
   babd8:	48 8d 7c 24 40       	lea    rdi,[rsp+0x40]
   babdd:	31 f6                	xor    esi,esi
   babdf:	e8 7c 90 f7 ff       	call   33c60 <lj_arena_hugetab_reader_release>
   babe4:	e9 a0 fd ff ff       	jmp    ba989 <lj_gc2_small_lease_try+0x69>
   babe9:	89 c8                	mov    eax,ecx
   babeb:	f3 48 0f bc ff       	tzcnt  rdi,rdi
   babf0:	8b 74 24 0c          	mov    esi,DWORD PTR [rsp+0xc]
   babf4:	83 e0 c0             	and    eax,0xffffffc0
   babf7:	01 f8                	add    eax,edi
   babf9:	39 c2                	cmp    edx,eax
   babfb:	0f 47 d0             	cmova  edx,eax
   babfe:	89 d0                	mov    eax,edx
   bac00:	44 29 e8             	sub    eax,r13d
   bac03:	48 c1 e0 04          	shl    rax,0x4
   bac07:	48 39 e8             	cmp    rax,rbp
   bac0a:	0f 82 5a fe ff ff    	jb     baa6a <lj_gc2_small_lease_try+0x14a>
   bac10:	45 85 e4             	test   r12d,r12d
   bac13:	74 17                	je     bac2c <lj_gc2_small_lease_try+0x30c>
   bac15:	48 83 fd 09          	cmp    rbp,0x9
   bac19:	0f 86 4b fe ff ff    	jbe    baa6a <lj_gc2_small_lease_try+0x14a>
   bac1f:	0f b6 53 09          	movzx  edx,BYTE PTR [rbx+0x9]
   bac23:	44 39 e2             	cmp    edx,r12d
   bac26:	0f 85 3e fe ff ff    	jne    baa6a <lj_gc2_small_lease_try+0x14a>
   bac2c:	49 8b 12             	mov    rdx,QWORD PTR [r10]
   bac2f:	48 0f a3 f2          	bt     rdx,rsi
   bac33:	0f 83 31 fe ff ff    	jae    baa6a <lj_gc2_small_lease_try+0x14a>
   bac39:	49 8b 13             	mov    rdx,QWORD PTR [r11]
   bac3c:	48 0f a3 f2          	bt     rdx,rsi
   bac40:	0f 82 24 fe ff ff    	jb     baa6a <lj_gc2_small_lease_try+0x14a>
   bac46:	49 8b 17             	mov    rdx,QWORD PTR [r15]
   bac49:	0f b6 4c 24 10       	movzx  ecx,BYTE PTR [rsp+0x10]
   bac4e:	48 d3 ea             	shr    rdx,cl
   bac51:	f7 d2                	not    edx
   bac53:	80 e2 03             	and    dl,0x3
   bac56:	0f 84 0e fe ff ff    	je     baa6a <lj_gc2_small_lease_try+0x14a>
   bac5c:	45 85 e4             	test   r12d,r12d
   bac5f:	74 3d                	je     bac9e <lj_gc2_small_lease_try+0x37e>
   bac61:	48 81 fb ff ff 00 00 	cmp    rbx,0xffff
   bac68:	0f 86 fc fd ff ff    	jbe    baa6a <lj_gc2_small_lease_try+0x14a>
   bac6e:	44 89 ea             	mov    edx,r13d
   bac71:	42 8d 0c ad 00 00 00 00 	lea    ecx,[r13*4+0x0]
   bac79:	c1 ea 04             	shr    edx,0x4
   bac7c:	83 e1 3c             	and    ecx,0x3c
   bac7f:	81 c2 10 02 00 00    	add    edx,0x210
   bac85:	49 8b 14 d6          	mov    rdx,QWORD PTR [r14+rdx*8]
   bac89:	48 d3 ea             	shr    rdx,cl
   bac8c:	b9 a6 01 00 00       	mov    ecx,0x1a6
   bac91:	83 e2 0f             	and    edx,0xf
   bac94:	48 0f a3 d1          	bt     rcx,rdx
   bac98:	0f 83 cc fd ff ff    	jae    baa6a <lj_gc2_small_lease_try+0x14a>
   bac9e:	48 8b 54 24 30       	mov    rdx,QWORD PTR [rsp+0x30]
   baca3:	48 63 4c 24 38       	movsxd rcx,DWORD PTR [rsp+0x38]
   baca8:	49 89 10             	mov    QWORD PTR [r8],rdx
   bacab:	49 89 48 08          	mov    QWORD PTR [r8+0x8],rcx
   bacaf:	81 f9 ce bc b8 ff    	cmp    ecx,0xffb8bcce
   bacb5:	75 14                	jne    baccb <lj_gc2_small_lease_try+0x3ab>
   bacb7:	48 8b 54 24 50       	mov    rdx,QWORD PTR [rsp+0x50]
   bacbc:	66 0f 6f 44 24 40    	movdqa xmm0,XMMWORD PTR [rsp+0x40]
   bacc2:	49 89 50 20          	mov    QWORD PTR [r8+0x20],rdx
   bacc6:	41 0f 11 40 10       	movups XMMWORD PTR [r8+0x10],xmm0
   baccb:	4d 85 c9             	test   r9,r9
   bacce:	74 03                	je     bacd3 <lj_gc2_small_lease_try+0x3b3>
   bacd0:	49 89 01             	mov    QWORD PTR [r9],rax
   bacd3:	b8 01 00 00 00       	mov    eax,0x1
   bacd8:	e9 ae fc ff ff       	jmp    ba98b <lj_gc2_small_lease_try+0x6b>

Disassembly of section .fini:
