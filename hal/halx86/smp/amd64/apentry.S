/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) long-mode boot entry
 * COPYRIGHT:   Copyright 2024 ReactOS Project
 *              Based on i386 AP entry by Victor Perevertkin and Justin Miller
 *
 * This whole block is copied verbatim into HalpLowStub (below 1 MiB) by
 * HalStartNextProcessor() and executed by the AP starting in real mode.
 * Because everything lives inside a single copied block, all cross references
 * inside it use rip-relative addressing or runtime-patched immediates.
 *
 * Ml64 notes: the assembler only understands 64-bit code, so the 16-bit and
 * 32-bit stages are hand-encoded machine code emitted with db/dw/dd/dq.
 * asm.inc must NOT be included: its .code16/.code32 macros expand to
 * ASSUME/use16/.586P which ml64 rejects.
 *
 * Layout of the copied block (relative to HalpAPEntry16):
 *   HalpAPEntry16        16-bit real mode stage
 *   HalpAPJump16         16-bit far jump immediate (offset32 + selector16)
 *   HalpAPEntryData      boot variables patched by the HAL
 *   HalpMinimalGdtDesc   6-byte lgdt operand for the minimal flat GDT
 *   HalpMinimalGdt       six 8-byte GDT entries
 *   HalpAPEntry32        32-bit protected mode stage
 *   HalpAPJump32         32-bit far jump immediate (offset32 + selector16)
 *   HalpAPEntry64        64-bit long mode stage
 *   HalpFullGdtDesc      scratch lgdt operand built at runtime
 *   HalpFullIdtDesc      scratch lidt operand built at runtime
 *   HalpAPEntry16End     end of the copied block
 */

PUBLIC HalpAPEntry16
PUBLIC HalpAPJump16
PUBLIC HalpAPEntryData
PUBLIC HalpMinimalGdtDesc
PUBLIC HalpMinimalGdt
PUBLIC HalpAPEntry32
PUBLIC HalpAPJump32
PUBLIC HalpAPEntry64
PUBLIC HalpAPEntry16End

OPTION DOTNAME

.code

/* ========================================================================
 * 16-bit real mode stage
 *
 * On APIC SIPI entry: CS = vector << 12, IP = 0, so the physical base of this
 * code is CS << 4. Everything below 1 MiB is identity mapped, and the minimal
 * GDT installed here has a zero base, so "ebp + offset" addresses the copied
 * block both before and after protected mode is enabled.
 *
 * Steps: mask interrupts, flatten segments, derive ebp, load the temporary
 * PAE CR3, enable PAE, turn on long mode in EFER, load the minimal GDT,
 * enable protected mode and far jump into the 32-bit stage.
 * ===================================================================== */
HalpAPEntry16:

    /* Progress marker: '0' on COM1 (very first instruction executed) */
    db 0BAh, 0F8h, 003h                             /* mov dx, 03F8h */
    db 0B0h, 030h                                   /* mov al, '0' */
    db 0EEh                                         /* out dx, al */

    /* cli */
    db 0FAh

    /* xor eax, eax ; mov ds/es/ss/fs/gs, ax */
    db 066h, 033h, 0C0h
    db 08Eh, 0D8h
    db 08Eh, 0C0h
    db 08Eh, 0D0h
    db 08Eh, 0E0h
    db 08Eh, 0E8h

    /* ebp = CS << 4 : movzx ebp, cs ; shl ebp, 4 */
    db 066h, 00Fh, 0B7h, 0ECh
    db 066h, 0C1h, 0E5h, 004h

    /* mov bx, offset of the minimal GDT descriptor ; lgdt [ebp + ebx]
     * lgdt is legal in real mode, but CR3/CR4/EFER are not: they are
     * programmed by the 32-bit stage once protected mode is active. */
    db 0BBh
    dw (HalpMinimalGdtDesc - HalpAPEntry16)
    db 067h, 00Fh, 001h, 094h, 01Dh, 000h, 000h, 000h, 000h

    /* Progress marker: emit '1' on COM1 (still real mode) */
    db 0BAh, 0F8h, 003h                             /* mov dx, 03F8h */
    db 0B0h, 031h                                   /* mov al, '1' */
    db 0EEh                                         /* out dx, al */

    /* Enable protected mode: mov eax, cr0 ; or eax, 1 ; mov cr0, eax */
    db 00Fh, 020h, 0C0h
    db 083h, 0C8h, 001h
    db 00Fh, 022h, 0C0h

    /* marker 'y': protected mode enabled */
    db 0B0h, 079h
    db 0EEh

    /* Far jump to the 32-bit stage: EA <offset32> <selector16 0x08>.
     * No 0x66 prefix: the cleared CS left by SIPI already has D=1 on this
     * CPU, so the default operand size is 32 bits and the offset below is
     * consumed as a full dword. The immediate is patched at runtime. */
    db 0EAh
HalpAPJump16:
    dd 000000000h
    dw 00008h

/* ------------------------------------------------------------------------
 * Boot variables filled in by HalStartNextProcessor().
 * Keep this layout in sync with AP_ENTRY_DATA_AMD64 in spinup.c.
 * The long mode stage only reads these fields, so it never depends on the
 * exact layout of KPROCESSOR_STATE.
 * --------------------------------------------------------------------- */
HalpAPEntryData:
    dq 00000000000000000h       /* +00 PageTableRoot32 : temporary PAE CR3     */
    dd 000000000h               /* +08 PaeCr4          : CR4 with PAE set      */
    dd 000000000h               /* +0C EferFlags       : EFER.LME | EFER.SCE   */
    dq 00000000000000000h       /* +10 PageTableRoot64 : long mode CR3         */
    dq 00000000000000000h       /* +18 Cr0                                     */
    dq 00000000000000000h       /* +20 BootCr3         : boot PML4 to clone    */
    dq 00000000000000000h       /* +28 Pcr             : PCR kernel VA         */
    dq 00000000000000000h       /* +30 KernelStack     : initial RSP           */
    dq 00000000000000000h       /* +38 EntryPoint      : initial RIP           */
    dq 00000000000000000h       /* +40 FlatBase        : physical base         */
    dq 00000000000000000h       /* +48 GdtrBase                                */
    dd 000000000h               /* +50 GdtrLimit                               */
    dd 000000000h               /* +54 (padding)                               */
    dq 00000000000000000h       /* +58 IdtrBase                                */
    dd 000000000h               /* +60 IdtrLimit                               */
    dd 000000000h               /* +64 (padding)                               */
    dd 000000000h               /* +68 TssSelector                             */
    dd 000000000h               /* +6C (padding)                               */

/* Minimal GDT: six flat entries, limit 6*8-1. The base is patched to the
 * physical address of the copied block since 16-bit lgdt takes a 32-bit base. */
HalpMinimalGdtDesc:
    dw 0002Fh
    dd 000000000h

HalpMinimalGdt:
    dq 00000000000000000h       /* 0x00 null                                  */
    dq 00C09A000000FFFFh        /* 0x08 32-bit code, base 0, limit 4 GiB      */
    dq 00C092000000FFFFh        /* 0x10 32-bit data, base 0, limit 4 GiB      */
    dq 00209A0000000001h        /* 0x18 64-bit code, long mode                */
    dq 0000920000000001h        /* 0x20 64-bit data                           */
    dq 00000000000000000h       /* 0x28 unused                                */

/* ========================================================================
 * 32-bit protected mode stage
 *
 * Entered with CS = 0x08, ebp = physical base of the block, paging still off.
 * Enable paging with the temporary PAE tables, then switch CR3 to the
 * temporary PML4 (which keeps the low identity mapping and clones the boot
 * processor's kernel address space) before jumping into long mode.
 * ===================================================================== */
HalpAPEntry32:

    /* marker 'w': entered 32-bit protected mode */
    db 0B0h, 077h
    db 0EEh

    /* mov eax, 0x10 ; mov ds/es/ss/fs/gs, ax */
    db 0B8h, 010h, 000h, 000h, 000h
    db 08Eh, 0D8h
    db 08Eh, 0C0h
    db 08Eh, 0D0h
    db 08Eh, 0E0h
    db 08Eh, 0E8h

    /* mov eax, [ebp + PaeCr4] ; mov cr4, eax  (enable PAE first) */
    db 08Bh, 085h
    dd (HalpAPEntryData - HalpAPEntry16) + 08h
    db 00Fh, 022h, 0E0h

    /* marker 'a': CR4 programmed */
    db 0B0h, 061h
    db 0EEh

    /* mov eax, [ebp + PageTableRoot32] ; mov cr3, eax (PAE PDPT) */
    db 08Bh, 085h
    dd (HalpAPEntryData - HalpAPEntry16) + 00h
    db 00Fh, 022h, 0D8h

    /* marker 'b': CR3 programmed */
    db 0B0h, 062h
    db 0EEh

    /* Set EFER.LME (long mode enable) before paging is turned on:
     *   mov ecx, 0xC0000080 ; rdmsr
     *   or  eax, [ebp + EferFlags] ; wrmsr */
    db 0B9h, 080h, 000h, 000h, 0C0h
    db 00Fh, 032h
    db 00Bh, 085h
    dd (HalpAPEntryData - HalpAPEntry16) + 0Ch
    db 00Fh, 030h

    /* marker 'c': EFER programmed */
    db 0B0h, 063h
    db 0EEh

    /* Enable paging: mov eax, cr0 ; or eax, 80000000h ; mov cr0, eax */
    db 00Fh, 020h, 0C0h
    db 00Dh, 000h, 000h, 000h, 080h
    db 00Fh, 022h, 0C0h

    /* marker 'd': paging enabled */
    db 0B0h, 064h
    db 0EEh

    /* Progress marker: '2' on COM1 (paging enabled) */
    db 0BAh, 0F8h, 003h, 000h, 000h                 /* mov edx, 03F8h */
    db 0B0h, 032h                                   /* mov al, '2' */
    db 0EEh                                         /* out dx, al */

    /* Far jump to the 64-bit stage: EA <offset32> <selector16 0x18>.
     * The offset is patched at runtime by the HAL.
     * CR3 still holds the 32-bit PAE PDPT here; it is replaced with the
     * temporary PML4 by the long mode stage. */
    db 0EAh
HalpAPJump32:
    dd 000000000h
    dw 00018h

/* ========================================================================
 * 64-bit long mode stage
 *
 * Entered with CS = 0x18, CR3 = temporary PAE PDPT (identity 0-4 GiB).
 * First switch to the temporary PML4 whose entry 0 wraps that same PDPT, then
 * clone the boot processor's PML4 entries 1..511 so the kernel address space
 * becomes visible. Entry 0 is left untouched, which keeps this code mapped
 * for the whole copy.
 * ===================================================================== */
HalpAPEntry64:

    /* r15 = address of the boot data inside the copied block */
    lea     r15, HalpAPEntryData

    /* Switch to the temporary PML4 (entry 0 identity maps the low 1 GiB) */
    mov     rbx, qword ptr [r15 + 10h]
    test    rbx, rbx
    jz      HaltNow
    mov     cr3, rbx

    /* Progress marker: '3' on COM1 (PML4 active) */
    mov     dx, 03F8h
    mov     al, 033h
    out     dx, al

    /* Copy the boot processor PML4 entries 1..511 into our PML4.
     * The source is the boot CR3 physical address, reachable because the low
     * 1 GiB are identity mapped by the PDPT installed above. */
    mov     rsi, qword ptr [r15 + 20h]      /* BootCr3 */
    shr     rsi, 12
    shl     rsi, 12
    mov     rdi, rbx
    add     rsi, 8
    add     rdi, 8
    mov     ecx, 511
CopyPml4:
    mov     rdx, qword ptr [rsi]
    mov     qword ptr [rdi], rdx
    add     rsi, 8
    add     rdi, 8
    dec     ecx
    jnz     CopyPml4

    /* Progress marker: '4' on COM1 (kernel PML4 cloned) */
    mov     dx, 03F8h
    mov     al, 034h
    out     dx, al

    /* Restore CR4 (PAE/PGE/OSFXSR/...) then CR0 (PE/PG/WP/...) */
    mov     rax, qword ptr [r15 + 08h]
    mov     cr4, rax
    mov     rax, qword ptr [r15 + 18h]
    mov     cr0, rax

    /* Program MSR_GS_BASE and MSR_GS_SWAP with this processor's PCR so that
     * KeGetCurrentPrcb() works as soon as we enter the kernel. */
    mov     rbx, qword ptr [r15 + 28h]
    test    rbx, rbx
    jz      HaltNow

    mov     r8d, 0C0000101h             /* MSR_GS_BASE */
    mov     ecx, r8d
    mov     rax, rbx
    mov     rdx, rax
    shr     rdx, 32
    wrmsr

    mov     r8d, 0C0000102h             /* MSR_GS_SWAP */
    mov     ecx, r8d
    mov     rax, rbx
    mov     rdx, rax
    shr     rdx, 32
    wrmsr

    /* Select the PCR descriptor (AMD64 ignores its base for GS) */
    mov     ax, 0002Bh
    mov     gs, ax

    /* Load the real GDT */
    lea     rdx, HalpFullGdtDesc
    mov     eax, dword ptr [r15 + 50h]
    mov     word ptr [rdx], ax
    mov     rax, qword ptr [r15 + 48h]
    mov     qword ptr [rdx + 2], rax
    lgdt    fword ptr [rdx]

    /* Load the real IDT */
    lea     rdx, HalpFullIdtDesc
    mov     eax, dword ptr [r15 + 60h]
    mov     word ptr [rdx], ax
    mov     rax, qword ptr [r15 + 58h]
    mov     qword ptr [rdx + 2], rax
    lidt    fword ptr [rdx]

    /* Load the task register */
    mov     eax, dword ptr [r15 + 68h]
    ltr     ax

    /* Switch to the per-processor kernel stack and enter the kernel */
    mov     rsp, qword ptr [r15 + 30h]
    mov     rax, qword ptr [r15 + 38h]

    /* Progress marker: '5' on COM1 (about to enter the kernel) */
    mov     dx, 03F8h
    mov     al, 035h
    out     dx, al

    jmp     rax

HaltNow:
    cli
.halt_loop:
    db 0F4h
    jmp     .halt_loop

/* Scratch lgdt/lidt operands built at runtime: 2-byte limit + 8-byte base */
HalpFullGdtDesc:
    dw 00000h
    dq 00000000000000000h

HalpFullIdtDesc:
    dw 00000h
    dq 00000000000000000h

/* End of the block copied into HalpLowStub */
HalpAPEntry16End:

END
