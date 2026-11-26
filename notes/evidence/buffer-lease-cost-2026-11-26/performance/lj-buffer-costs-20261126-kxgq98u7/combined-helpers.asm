
/tmp/lj-buffer-perf-combined-20261126-payfa8fl/candidate-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000ca6f0 <bufx_pin_try>:
   ca6f0:	41 57                	push   r15
   ca6f2:	41 56                	push   r14
   ca6f4:	49 89 fe             	mov    r14,rdi
   ca6f7:	41 55                	push   r13
   ca6f9:	41 54                	push   r12
   ca6fb:	49 89 f4             	mov    r12,rsi
   ca6fe:	55                   	push   rbp
   ca6ff:	48 89 cd             	mov    rbp,rcx
   ca702:	b9 0e 00 00 00       	mov    ecx,0xe
   ca707:	53                   	push   rbx
   ca708:	31 db                	xor    ebx,ebx
   ca70a:	48 89 ef             	mov    rdi,rbp
   ca70d:	48 89 d8             	mov    rax,rbx
   ca710:	48 83 ec 28          	sub    rsp,0x28
   ca714:	f3 48 ab             	rep stos QWORD PTR es:[rdi],rax
   ca717:	48 8b 36             	mov    rsi,QWORD PTR [rsi]
   ca71a:	48 89 75 10          	mov    QWORD PTR [rbp+0x10],rsi
   ca71e:	48 85 f6             	test   rsi,rsi
   ca721:	75 1d                	jne    ca740 <bufx_pin_try+0x50>
   ca723:	48 83 c4 28          	add    rsp,0x28
   ca727:	b8 01 00 00 00       	mov    eax,0x1
   ca72c:	5b                   	pop    rbx
   ca72d:	5d                   	pop    rbp
   ca72e:	41 5c                	pop    r12
   ca730:	41 5d                	pop    r13
   ca732:	41 5e                	pop    r14
   ca734:	41 5f                	pop    r15
   ca736:	c3                   	ret
   ca737:	66 0f 1f 84 00 00 00 00 00 	nop    WORD PTR [rax+rax*1+0x0]
   ca740:	4c 8d 6d 18          	lea    r13,[rbp+0x18]
   ca744:	49 8b 7e 10          	mov    rdi,QWORD PTR [r14+0x10]
   ca748:	41 89 d7             	mov    r15d,edx
   ca74b:	45 31 c0             	xor    r8d,r8d
   ca74e:	4c 89 e9             	mov    rcx,r13
   ca751:	ba 30 00 00 00       	mov    edx,0x30
   ca756:	48 89 74 24 08       	mov    QWORD PTR [rsp+0x8],rsi
   ca75b:	e8 20 06 ff ff       	call   bad80 <lj_gc2_storage_lease_try>
   ca760:	85 c0                	test   eax,eax
   ca762:	74 35                	je     ca799 <bufx_pin_try+0xa9>
   ca764:	48 89 d8             	mov    rax,rbx
   ca767:	48 89 da             	mov    rdx,rbx
   ca76a:	48 89 d9             	mov    rcx,rbx
   ca76d:	f0 49 0f c7 0c 24    	lock cmpxchg16b OWORD PTR [r12]
   ca773:	48 8b 74 24 08       	mov    rsi,QWORD PTR [rsp+0x8]
   ca778:	48 89 45 00          	mov    QWORD PTR [rbp+0x0],rax
   ca77c:	4c 8d 45 40          	lea    r8,[rbp+0x40]
   ca780:	48 89 55 08          	mov    QWORD PTR [rbp+0x8],rdx
   ca784:	48 39 f0             	cmp    rax,rsi
   ca787:	74 27                	je     ca7b0 <bufx_pin_try+0xc0>
   ca789:	4c 89 c7             	mov    rdi,r8
   ca78c:	e8 7f 01 ff ff       	call   ba910 <lj_gc2_lease_release>
   ca791:	4c 89 ef             	mov    rdi,r13
   ca794:	e8 77 01 ff ff       	call   ba910 <lj_gc2_lease_release>
   ca799:	48 83 c4 28          	add    rsp,0x28
   ca79d:	31 c0                	xor    eax,eax
   ca79f:	5b                   	pop    rbx
   ca7a0:	5d                   	pop    rbp
   ca7a1:	41 5c                	pop    r12
   ca7a3:	41 5d                	pop    r13
   ca7a5:	41 5e                	pop    r14
   ca7a7:	41 5f                	pop    r15
   ca7a9:	c3                   	ret
   ca7aa:	66 0f 1f 44 00 00    	nop    WORD PTR [rax+rax*1+0x0]
   ca7b0:	8b 70 28             	mov    esi,DWORD PTR [rax+0x28]
   ca7b3:	48 8b 55 08          	mov    rdx,QWORD PTR [rbp+0x8]
   ca7b7:	48 89 d1             	mov    rcx,rdx
   ca7ba:	48 c1 e9 20          	shr    rcx,0x20
   ca7be:	39 f1                	cmp    ecx,esi
   ca7c0:	73 c7                	jae    ca789 <bufx_pin_try+0x99>
   ca7c2:	45 85 ff             	test   r15d,r15d
   ca7c5:	0f 84 58 ff ff ff    	je     ca723 <bufx_pin_try+0x33>
   ca7cb:	48 8b 70 10          	mov    rsi,QWORD PTR [rax+0x10]
   ca7cf:	48 85 f6             	test   rsi,rsi
   ca7d2:	0f 84 4b ff ff ff    	je     ca723 <bufx_pin_try+0x33>
   ca7d8:	39 d1                	cmp    ecx,edx
   ca7da:	0f 84 43 ff ff ff    	je     ca723 <bufx_pin_try+0x33>
   ca7e0:	8b 50 24             	mov    edx,DWORD PTR [rax+0x24]
   ca7e3:	f7 d2                	not    edx
   ca7e5:	48 c1 e2 2f          	shl    rdx,0x2f
   ca7e9:	48 09 d6             	or     rsi,rdx
   ca7ec:	48 89 74 24 18       	mov    QWORD PTR [rsp+0x18],rsi
   ca7f1:	83 78 24 04          	cmp    DWORD PTR [rax+0x24],0x4
   ca7f5:	74 4e                	je     ca845 <bufx_pin_try+0x155>
   ca7f7:	49 8b 7e 10          	mov    rdi,QWORD PTR [r14+0x10]
   ca7fb:	4c 89 c2             	mov    rdx,r8
   ca7fe:	48 8d 74 24 18       	lea    rsi,[rsp+0x18]
   ca803:	4c 89 44 24 08       	mov    QWORD PTR [rsp+0x8],r8
   ca808:	e8 23 00 ff ff       	call   ba830 <lj_gc2_tv_lease_acquire>
   ca80d:	4c 8b 44 24 08       	mov    r8,QWORD PTR [rsp+0x8]
   ca812:	83 f8 01             	cmp    eax,0x1
   ca815:	0f 85 6e ff ff ff    	jne    ca789 <bufx_pin_try+0x99>
   ca81b:	31 c0                	xor    eax,eax
   ca81d:	48 89 c2             	mov    rdx,rax
   ca820:	48 89 c3             	mov    rbx,rax
   ca823:	48 89 c1             	mov    rcx,rax
   ca826:	f0 49 0f c7 0c 24    	lock cmpxchg16b OWORD PTR [r12]
   ca82c:	48 39 45 00          	cmp    QWORD PTR [rbp+0x0],rax
   ca830:	0f 85 53 ff ff ff    	jne    ca789 <bufx_pin_try+0x99>
   ca836:	48 39 55 08          	cmp    QWORD PTR [rbp+0x8],rdx
   ca83a:	0f 85 49 ff ff ff    	jne    ca789 <bufx_pin_try+0x99>
   ca840:	e9 de fe ff ff       	jmp    ca723 <bufx_pin_try+0x33>
   ca845:	48 8b 70 10          	mov    rsi,QWORD PTR [rax+0x10]
   ca849:	49 8b 7e 10          	mov    rdi,QWORD PTR [r14+0x10]
   ca84d:	45 31 c9             	xor    r9d,r9d
   ca850:	b9 04 00 00 00       	mov    ecx,0x4
   ca855:	ba 30 00 00 00       	mov    edx,0x30
   ca85a:	4c 89 44 24 08       	mov    QWORD PTR [rsp+0x8],r8
   ca85f:	e8 3c 01 ff ff       	call   ba9a0 <lj_gc2_small_lease_try>
   ca864:	4c 8b 44 24 08       	mov    r8,QWORD PTR [rsp+0x8]
   ca869:	85 c0                	test   eax,eax
   ca86b:	74 8a                	je     ca7f7 <bufx_pin_try+0x107>
   ca86d:	eb ac                	jmp    ca81b <bufx_pin_try+0x12b>

Disassembly of section .fini:

/tmp/lj-buffer-perf-combined-20261126-payfa8fl/candidate-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cbbc0 <lj_bufx_append>:
   cbbc0:	41 57                	push   r15
   cbbc2:	41 56                	push   r14
   cbbc4:	41 55                	push   r13
   cbbc6:	41 89 cd             	mov    r13d,ecx
   cbbc9:	41 54                	push   r12
   cbbcb:	55                   	push   rbp
   cbbcc:	48 89 f5             	mov    rbp,rsi
   cbbcf:	53                   	push   rbx
   cbbd0:	48 89 fb             	mov    rbx,rdi
   cbbd3:	48 81 ec 98 00 00 00 	sub    rsp,0x98
   cbbda:	48 89 54 24 08       	mov    QWORD PTR [rsp+0x8],rdx
   cbbdf:	4c 8d 64 24 20       	lea    r12,[rsp+0x20]
   cbbe4:	85 c9                	test   ecx,ecx
   cbbe6:	75 10                	jne    cbbf8 <lj_bufx_append+0x38>
   cbbe8:	e9 22 01 00 00       	jmp    cbd0f <lj_bufx_append+0x14f>
   cbbed:	0f 1f 00             	nop    DWORD PTR [rax]
   cbbf0:	48 89 df             	mov    rdi,rbx
   cbbf3:	e8 b8 68 00 00       	call   d24b0 <lj_tab_wait_l>
   cbbf8:	4c 89 e1             	mov    rcx,r12
   cbbfb:	ba 01 00 00 00       	mov    edx,0x1
   cbc00:	48 89 ee             	mov    rsi,rbp
   cbc03:	48 89 df             	mov    rdi,rbx
   cbc06:	e8 e5 ea ff ff       	call   ca6f0 <bufx_pin_try>
   cbc0b:	85 c0                	test   eax,eax
   cbc0d:	74 e1                	je     cbbf0 <lj_bufx_append+0x30>
   cbc0f:	48 8b 54 24 30       	mov    rdx,QWORD PTR [rsp+0x30]
   cbc14:	48 85 d2             	test   rdx,rdx
   cbc17:	74 0b                	je     cbc24 <lj_bufx_append+0x64>
   cbc19:	48 83 7a 10 00       	cmp    QWORD PTR [rdx+0x10],0x0
   cbc1e:	0f 84 7c 00 00 00    	je     cbca0 <lj_bufx_append+0xe0>
   cbc24:	4c 8d 44 24 1c       	lea    r8,[rsp+0x1c]
   cbc29:	44 89 e9             	mov    ecx,r13d
   cbc2c:	4c 89 e2             	mov    rdx,r12
   cbc2f:	48 89 ee             	mov    rsi,rbp
   cbc32:	48 89 df             	mov    rdi,rbx
   cbc35:	e8 66 f0 ff ff       	call   caca0 <bufx_copy_store>
   cbc3a:	49 89 c7             	mov    r15,rax
   cbc3d:	48 85 c0             	test   rax,rax
   cbc40:	0f 84 db 00 00 00    	je     cbd21 <lj_bufx_append+0x161>
   cbc46:	8b 44 24 1c          	mov    eax,DWORD PTR [rsp+0x1c]
   cbc4a:	48 8b 74 24 08       	mov    rsi,QWORD PTR [rsp+0x8]
   cbc4f:	44 89 ea             	mov    edx,r13d
   cbc52:	49 89 c6             	mov    r14,rax
   cbc55:	49 8d 7c 07 30       	lea    rdi,[r15+rax*1+0x30]
   cbc5a:	e8 91 da f3 ff       	call   96f0 <memcpy@plt>
   cbc5f:	43 8d 04 2e          	lea    eax,[r14+r13*1]
   cbc63:	4c 89 f9             	mov    rcx,r15
   cbc66:	4c 89 e2             	mov    rdx,r12
   cbc69:	49 89 47 08          	mov    QWORD PTR [r15+0x8],rax
   cbc6d:	49 89 c0             	mov    r8,rax
   cbc70:	48 89 ee             	mov    rsi,rbp
   cbc73:	48 89 df             	mov    rdi,rbx
   cbc76:	e8 f5 eb ff ff       	call   ca870 <bufx_replace>
   cbc7b:	85 c0                	test   eax,eax
   cbc7d:	0f 85 8c 00 00 00    	jne    cbd0f <lj_bufx_append+0x14f>
   cbc83:	48 8d 7c 24 60       	lea    rdi,[rsp+0x60]
   cbc88:	e8 83 ec fe ff       	call   ba910 <lj_gc2_lease_release>
   cbc8d:	48 8d 7c 24 38       	lea    rdi,[rsp+0x38]
   cbc92:	e8 79 ec fe ff       	call   ba910 <lj_gc2_lease_release>
   cbc97:	e9 5c ff ff ff       	jmp    cbbf8 <lj_bufx_append+0x38>
   cbc9c:	0f 1f 40 00          	nop    DWORD PTR [rax+0x0]
   cbca0:	44 8b 7c 24 2c       	mov    r15d,DWORD PTR [rsp+0x2c]
   cbca5:	8b 42 20             	mov    eax,DWORD PTR [rdx+0x20]
   cbca8:	44 29 f8             	sub    eax,r15d
   cbcab:	44 39 e8             	cmp    eax,r13d
   cbcae:	0f 82 70 ff ff ff    	jb     cbc24 <lj_bufx_append+0x64>
   cbcb4:	48 b9 00 00 00 00 01 00 00 00 	movabs rcx,0x100000000
   cbcbe:	4c 89 f8             	mov    rax,r15
   cbcc1:	4c 09 f9             	or     rcx,r15
   cbcc4:	f0 48 0f b1 4a 08    	lock cmpxchg QWORD PTR [rdx+0x8],rcx
   cbcca:	0f 85 54 ff ff ff    	jne    cbc24 <lj_bufx_append+0x64>
   cbcd0:	48 8b 44 24 30       	mov    rax,QWORD PTR [rsp+0x30]
   cbcd5:	48 8b 74 24 08       	mov    rsi,QWORD PTR [rsp+0x8]
   cbcda:	44 89 ea             	mov    edx,r13d
   cbcdd:	4a 8d 7c 38 30       	lea    rdi,[rax+r15*1+0x30]
   cbce2:	e8 09 da f3 ff       	call   96f0 <memcpy@plt>
   cbce7:	43 8d 54 3d 00       	lea    edx,[r13+r15*1+0x0]
   cbcec:	4c 89 e6             	mov    rsi,r12
   cbcef:	48 89 ef             	mov    rdi,rbp
   cbcf2:	e8 99 e9 ff ff       	call   ca690 <bufx_endwrite>
   cbcf7:	85 c0                	test   eax,eax
   cbcf9:	74 88                	je     cbc83 <lj_bufx_append+0xc3>
   cbcfb:	48 8d 7c 24 60       	lea    rdi,[rsp+0x60]
   cbd00:	e8 0b ec fe ff       	call   ba910 <lj_gc2_lease_release>
   cbd05:	48 8d 7c 24 38       	lea    rdi,[rsp+0x38]
   cbd0a:	e8 01 ec fe ff       	call   ba910 <lj_gc2_lease_release>
   cbd0f:	48 81 c4 98 00 00 00 	add    rsp,0x98
   cbd16:	5b                   	pop    rbx
   cbd17:	5d                   	pop    rbp
   cbd18:	41 5c                	pop    r12
   cbd1a:	41 5d                	pop    r13
   cbd1c:	41 5e                	pop    r14
   cbd1e:	41 5f                	pop    r15
   cbd20:	c3                   	ret
   cbd21:	48 8d 7c 24 60       	lea    rdi,[rsp+0x60]
   cbd26:	e8 e5 eb fe ff       	call   ba910 <lj_gc2_lease_release>
   cbd2b:	48 8d 7c 24 38       	lea    rdi,[rsp+0x38]
   cbd30:	e8 db eb fe ff       	call   ba910 <lj_gc2_lease_release>
   cbd35:	48 89 df             	mov    rdi,rbx
   cbd38:	e8 a3 2d f4 ff       	call   eae0 <lj_err_mem>

Disassembly of section .fini:

/tmp/lj-buffer-perf-combined-20261126-payfa8fl/candidate-luajit:     file format elf64-x86-64


Disassembly of section .init:

Disassembly of section .plt:

Disassembly of section .plt.got:

Disassembly of section .text:

00000000000cbd40 <lj_bufx_putstr>:
   cbd40:	41 56                	push   r14
   cbd42:	41 55                	push   r13
   cbd44:	41 54                	push   r12
   cbd46:	49 89 f4             	mov    r12,rsi
   cbd49:	48 83 ee 30          	sub    rsi,0x30
   cbd4d:	55                   	push   rbp
   cbd4e:	48 89 d5             	mov    rbp,rdx
   cbd51:	ba f3 ff ff ff       	mov    edx,0xfffffff3
   cbd56:	53                   	push   rbx
   cbd57:	48 89 fb             	mov    rbx,rdi
   cbd5a:	e8 31 ed ff ff       	call   caa90 <bufx_anchor>
   cbd5f:	ba fb ff ff ff       	mov    edx,0xfffffffb
   cbd64:	48 89 ee             	mov    rsi,rbp
   cbd67:	48 89 df             	mov    rdi,rbx
   cbd6a:	41 89 c5             	mov    r13d,eax
   cbd6d:	e8 1e ed ff ff       	call   caa90 <bufx_anchor>
   cbd72:	8b 4d 14             	mov    ecx,DWORD PTR [rbp+0x14]
   cbd75:	48 89 df             	mov    rdi,rbx
   cbd78:	48 8d 55 30          	lea    rdx,[rbp+0x30]
   cbd7c:	4c 89 e6             	mov    rsi,r12
   cbd7f:	41 89 c6             	mov    r14d,eax
   cbd82:	e8 39 fe ff ff       	call   cbbc0 <lj_bufx_append>
   cbd87:	48 8b 7b 78          	mov    rdi,QWORD PTR [rbx+0x78]
   cbd8b:	48 85 ff             	test   rdi,rdi
   cbd8e:	74 28                	je     cbdb8 <lj_bufx_putstr+0x78>
   cbd90:	44 89 f6             	mov    esi,r14d
   cbd93:	e8 a8 6f f5 ff       	call   22d40 <lj_tg_root_anchor_pop>
   cbd98:	48 8b 7b 78          	mov    rdi,QWORD PTR [rbx+0x78]
   cbd9c:	48 85 ff             	test   rdi,rdi
   cbd9f:	74 2f                	je     cbdd0 <lj_bufx_putstr+0x90>
   cbda1:	5b                   	pop    rbx
   cbda2:	44 89 ee             	mov    esi,r13d
   cbda5:	5d                   	pop    rbp
   cbda6:	41 5c                	pop    r12
   cbda8:	41 5d                	pop    r13
   cbdaa:	41 5e                	pop    r14
   cbdac:	e9 8f 6f f5 ff       	jmp    22d40 <lj_tg_root_anchor_pop>
   cbdb1:	0f 1f 80 00 00 00 00 	nop    DWORD PTR [rax+0x0]
   cbdb8:	48 8b 7b 10          	mov    rdi,QWORD PTR [rbx+0x10]
   cbdbc:	e8 1f 9e f5 ff       	call   25be0 <lj_thr_get_tg_fallback>
   cbdc1:	48 89 c7             	mov    rdi,rax
   cbdc4:	eb ca                	jmp    cbd90 <lj_bufx_putstr+0x50>
   cbdc6:	66 2e 0f 1f 84 00 00 00 00 00 	cs nop WORD PTR [rax+rax*1+0x0]
   cbdd0:	48 8b 7b 10          	mov    rdi,QWORD PTR [rbx+0x10]
   cbdd4:	e8 07 9e f5 ff       	call   25be0 <lj_thr_get_tg_fallback>
   cbdd9:	5b                   	pop    rbx
   cbdda:	44 89 ee             	mov    esi,r13d
   cbddd:	5d                   	pop    rbp
   cbdde:	48 89 c7             	mov    rdi,rax
   cbde1:	41 5c                	pop    r12
   cbde3:	41 5d                	pop    r13
   cbde5:	41 5e                	pop    r14
   cbde7:	e9 54 6f f5 ff       	jmp    22d40 <lj_tg_root_anchor_pop>

Disassembly of section .fini:

/tmp/lj-buffer-perf-combined-20261126-payfa8fl/candidate-luajit:     file format elf64-x86-64


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
   baaf7:	0f 84 53 01 00 00    	je     bac50 <lj_gc2_small_lease_try+0x2b0>
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
   babd2:	0f 85 7b 01 00 00    	jne    bad53 <lj_gc2_small_lease_try+0x3b3>
   babd8:	4d 85 c9             	test   r9,r9
   babdb:	b8 00 40 00 00       	mov    eax,0x4000
   babe0:	89 74 24 0c          	mov    DWORD PTR [rsp+0xc],esi
   babe4:	41 8d 4d 01          	lea    ecx,[r13+0x1]
   babe8:	48 0f 44 c5          	cmove  rax,rbp
   babec:	ba 00 10 00 00       	mov    edx,0x1000
   babf1:	48 83 c0 0f          	add    rax,0xf
   babf5:	48 c1 e8 04          	shr    rax,0x4
   babf9:	44 01 e8             	add    eax,r13d
   babfc:	39 d0                	cmp    eax,edx
   babfe:	0f 47 c2             	cmova  eax,edx
   bac01:	eb 3b                	jmp    bac3e <lj_gc2_small_lease_try+0x29e>
   bac03:	89 cf                	mov    edi,ecx
   bac05:	c1 ef 06             	shr    edi,0x6
   bac08:	89 7c 24 18          	mov    DWORD PTR [rsp+0x18],edi
   bac0c:	83 c7 10             	add    edi,0x10
   bac0f:	49 8b 34 fe          	mov    rsi,QWORD PTR [r14+rdi*8]
   bac13:	48 8d 14 fd 00 00 00 00 	lea    rdx,[rdi*8+0x0]
   bac1b:	48 83 cf ff          	or     rdi,0xffffffffffffffff
   bac1f:	48 d3 e7             	shl    rdi,cl
   bac22:	49 8b 94 16 00 02 00 00 	mov    rdx,QWORD PTR [r14+rdx*1+0x200]
   bac2a:	48 09 f2             	or     rdx,rsi
   bac2d:	48 21 d7             	and    rdi,rdx
   bac30:	75 2f                	jne    bac61 <lj_gc2_small_lease_try+0x2c1>
   bac32:	8b 54 24 18          	mov    edx,DWORD PTR [rsp+0x18]
   bac36:	83 c2 01             	add    edx,0x1
   bac39:	89 d1                	mov    ecx,edx
   bac3b:	c1 e1 06             	shl    ecx,0x6
   bac3e:	39 c1                	cmp    ecx,eax
   bac40:	72 c1                	jb     bac03 <lj_gc2_small_lease_try+0x263>
   bac42:	8b 74 24 0c          	mov    esi,DWORD PTR [rsp+0xc]
   bac46:	eb 2e                	jmp    bac76 <lj_gc2_small_lease_try+0x2d6>
   bac48:	0f 1f 84 00 00 00 00 00 	nop    DWORD PTR [rax+rax*1+0x0]
   bac50:	48 8d 7c 24 40       	lea    rdi,[rsp+0x40]
   bac55:	31 f6                	xor    esi,esi
   bac57:	e8 04 90 f7 ff       	call   33c60 <lj_arena_hugetab_reader_release>
   bac5c:	e9 a8 fd ff ff       	jmp    baa09 <lj_gc2_small_lease_try+0x69>
   bac61:	83 e1 c0             	and    ecx,0xffffffc0
   bac64:	f3 48 0f bc ff       	tzcnt  rdi,rdi
   bac69:	8b 74 24 0c          	mov    esi,DWORD PTR [rsp+0xc]
   bac6d:	89 ca                	mov    edx,ecx
   bac6f:	01 fa                	add    edx,edi
   bac71:	39 d0                	cmp    eax,edx
   bac73:	0f 47 c2             	cmova  eax,edx
   bac76:	44 29 e8             	sub    eax,r13d
   bac79:	48 c1 e0 04          	shl    rax,0x4
   bac7d:	48 39 e8             	cmp    rax,rbp
   bac80:	0f 82 68 fe ff ff    	jb     baaee <lj_gc2_small_lease_try+0x14e>
   bac86:	45 85 e4             	test   r12d,r12d
   bac89:	74 17                	je     baca2 <lj_gc2_small_lease_try+0x302>
   bac8b:	48 83 fd 09          	cmp    rbp,0x9
   bac8f:	0f 86 59 fe ff ff    	jbe    baaee <lj_gc2_small_lease_try+0x14e>
   bac95:	0f b6 53 09          	movzx  edx,BYTE PTR [rbx+0x9]
   bac99:	44 39 e2             	cmp    edx,r12d
   bac9c:	0f 85 4c fe ff ff    	jne    baaee <lj_gc2_small_lease_try+0x14e>
   baca2:	49 8b 12             	mov    rdx,QWORD PTR [r10]
   baca5:	48 0f a3 f2          	bt     rdx,rsi
   baca9:	0f 83 3f fe ff ff    	jae    baaee <lj_gc2_small_lease_try+0x14e>
   bacaf:	49 8b 13             	mov    rdx,QWORD PTR [r11]
   bacb2:	48 0f a3 f2          	bt     rdx,rsi
   bacb6:	0f 82 32 fe ff ff    	jb     baaee <lj_gc2_small_lease_try+0x14e>
   bacbc:	49 8b 17             	mov    rdx,QWORD PTR [r15]
   bacbf:	0f b6 4c 24 10       	movzx  ecx,BYTE PTR [rsp+0x10]
   bacc4:	48 d3 ea             	shr    rdx,cl
   bacc7:	f7 d2                	not    edx
   bacc9:	80 e2 03             	and    dl,0x3
   baccc:	0f 84 1c fe ff ff    	je     baaee <lj_gc2_small_lease_try+0x14e>
   bacd2:	45 85 e4             	test   r12d,r12d
   bacd5:	74 3d                	je     bad14 <lj_gc2_small_lease_try+0x374>
   bacd7:	48 81 fb ff ff 00 00 	cmp    rbx,0xffff
   bacde:	0f 86 0a fe ff ff    	jbe    baaee <lj_gc2_small_lease_try+0x14e>
   bace4:	44 89 ea             	mov    edx,r13d
   bace7:	42 8d 0c ad 00 00 00 00 	lea    ecx,[r13*4+0x0]
   bacef:	c1 ea 04             	shr    edx,0x4
   bacf2:	83 e1 3c             	and    ecx,0x3c
   bacf5:	81 c2 10 02 00 00    	add    edx,0x210
   bacfb:	49 8b 14 d6          	mov    rdx,QWORD PTR [r14+rdx*8]
   bacff:	48 d3 ea             	shr    rdx,cl
   bad02:	b9 a6 01 00 00       	mov    ecx,0x1a6
   bad07:	83 e2 0f             	and    edx,0xf
   bad0a:	48 0f a3 d1          	bt     rcx,rdx
   bad0e:	0f 83 da fd ff ff    	jae    baaee <lj_gc2_small_lease_try+0x14e>
   bad14:	48 8b 54 24 30       	mov    rdx,QWORD PTR [rsp+0x30]
   bad19:	48 63 4c 24 38       	movsxd rcx,DWORD PTR [rsp+0x38]
   bad1e:	49 89 10             	mov    QWORD PTR [r8],rdx
   bad21:	49 89 48 08          	mov    QWORD PTR [r8+0x8],rcx
   bad25:	81 f9 ce bc b8 ff    	cmp    ecx,0xffb8bcce
   bad2b:	75 14                	jne    bad41 <lj_gc2_small_lease_try+0x3a1>
   bad2d:	48 8b 54 24 50       	mov    rdx,QWORD PTR [rsp+0x50]
   bad32:	66 0f 6f 44 24 40    	movdqa xmm0,XMMWORD PTR [rsp+0x40]
   bad38:	49 89 50 20          	mov    QWORD PTR [r8+0x20],rdx
   bad3c:	41 0f 11 40 10       	movups XMMWORD PTR [r8+0x10],xmm0
   bad41:	4d 85 c9             	test   r9,r9
   bad44:	74 03                	je     bad49 <lj_gc2_small_lease_try+0x3a9>
   bad46:	49 89 01             	mov    QWORD PTR [r9],rax
   bad49:	b8 01 00 00 00       	mov    eax,0x1
   bad4e:	e9 b8 fc ff ff       	jmp    baa0b <lj_gc2_small_lease_try+0x6b>
   bad53:	49 8b 94 06 80 24 00 00 	mov    rdx,QWORD PTR [r14+rax*1+0x2480]
   bad5b:	48 0f a3 f2          	bt     rdx,rsi
   bad5f:	0f 83 89 fd ff ff    	jae    baaee <lj_gc2_small_lease_try+0x14e>
   bad65:	49 8b 84 06 80 22 00 00 	mov    rax,QWORD PTR [r14+rax*1+0x2280]
   bad6d:	48 0f a3 f0          	bt     rax,rsi
   bad71:	0f 83 61 fe ff ff    	jae    babd8 <lj_gc2_small_lease_try+0x238>
   bad77:	e9 72 fd ff ff       	jmp    baaee <lj_gc2_small_lease_try+0x14e>

Disassembly of section .fini:
