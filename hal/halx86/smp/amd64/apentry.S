/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) long-mode boot entry
 * COPYRIGHT:   Copyright 2024 ReactOS Project
 *              Based on i386 AP entry by Victor Perevertkin and Justin Miller
 *
 * This whole block is copied verbatim into HalpLowStub (below 1 MiB) by
 * HalStartNextProcessor() and executed by the AP starting in real mode.
 * Because everything lives inside a single copied block, all references to it
 * use either CS-relative offsets or runtime-patched immediates.
 *
 * ml64 only understands 64-bit code, so the 16-bit and 32-bit stages are
 * hand-encoded machine code emitted with db/dw/dd/dq. asm.inc must NOT be
 * included: its .code16/.code32 macros expand to ASSUME/use16/.586P which
 * ml64 rejects.
 *
 * The flow mirrors the working i386 trampoline: the real mode stage programs
 * CR4/CR3/EFER and enables paging and protection at once, then far jumps into
 * the 32-bit stage, which only has to fix up the segment registers before
 * entering long mode.
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
PUBLIC HalpAPEntry64
PUBLIC HalpAPEntry16End

OPTION DOTNAME

.code

/* ========================================================================
 * 16-bit real mode stage
 *
 * On APIC SIPI entry: CS = vector << 12, IP = 0, so the physical base of this
 * code is CS << 4. Everything below 1 MiB is identity mapped by the temporary
 * tables, and the minimal GDT installed here has a zero base.
 *
 * All boot variables are read with a CS-relative offset, which is the most
 * robust way to touch the copied block: it does not depend on DS or on any
 * general purpose register surviving the SIPI reset.
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

    /* ebp = CS << 4, kept for the later stages */
    db 066h, 00Fh, 0B7h, 0ECh                       /* movzx ebp, cs */
    db 066h, 0C1h, 0E5h, 004h                       /* shl ebp, 4 */

    /* lgdt fword ptr cs:[HalpMinimalGdtDesc] */
    db 02Eh, 00Fh, 001h, 016h
    dw (HalpMinimalGdtDesc - HalpAPEntry16)

    /* Progress marker: '1' on COM1 (GDT loaded) */
    db 0B0h, 031h                                   /* mov al, '1' */
    db 0EEh                                         /* out dx, al */

    /* mov eax, cs:[PaeCr4] ; mov cr4, eax
     * The 0x66 prefixes are mandatory: in 16-bit mode "A1" alone would only
     * load AX and leave the upper half of EAX stale. */
    db 066h, 02Eh, 0A1h
    dw (HalpAPEntryData - HalpAPEntry16) + 08h
    db 00Fh, 022h, 0E0h

    /* Progress marker: 'a' on COM1 (CR4 programmed) */
    db 0B0h, 061h
    db 0EEh

    /* mov eax, cs:[PageTableRoot32] ; mov cr3, eax */
    db 066h, 02Eh, 0A1h
    dw (HalpAPEntryData - HalpAPEntry16) + 00h
    db 00Fh, 022h, 0D8h

    /* Progress marker: 'b' on COM1 (CR3 programmed) */
    db 0B0h, 062h
    db 0EEh

    /* EFER = EferFlags, which the HAL precomputes as the boot EFER value with
     * LME and SCE already set. Writing it directly avoids a read-modify-write
     * in 16-bit mode:
     *   mov ecx, 0xC0000080 ; mov eax, cs:[EferFlags]
     *   xor edx, edx ; wrmsr */
    db 066h, 0B9h, 080h, 000h, 000h, 0C0h
    db 066h, 02Eh, 0A1h
    dw (HalpAPEntryData - HalpAPEntry16) + 0Ch
    db 066h, 031h, 0D2h                             /* xor edx, edx */
    db 00Fh, 030h                                   /* wrmsr */

    /* EDX is now zero (it is the high half of the MSR value), so the COM1
     * port has to be reloaded before the next marker. */
    db 0BAh, 0F8h, 003h                             /* mov dx, 03F8h */
    db 0B0h, 063h, 0EEh                             /* 'c' */

    /* Enable paging and protection in one step, like the i386 trampoline:
     * mov eax, cr0 ; or eax, 80000001h ; mov cr0, eax */
    db 00Fh, 020h, 0C0h
    db 066h, 00Dh, 000h, 000h, 000h, 080h
    db 00Fh, 022h, 0C0h

    /* Progress marker: 'd' on COM1 (paging enabled) */
    db 0B0h, 064h
    db 0EEh

    /* Far jump straight into the 64-bit stage: 66 EA <offset32> <selector16>.
     * With paging and EFER.LME already active the CPU is in IA-32e, and a far
     * jump to a L=1 descriptor switches to 64-bit mode. Going through a 32-bit
     * compatibility segment is avoided on purpose.
     * The immediate is patched at runtime by the HAL. */
    db 066h, 0EAh
HalpAPJump16:
    dd 000000000h
    dw 00018h

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
 * physical address of the GDT entries themselves. */
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
 * 64-bit long mode stage
 *
 * Entered directly from real mode with CS = 0x18 (long mode descriptor), CR3
 * pointing at the temporary PAE PDPT and EFER.LME already set.
 * ===================================================================== */
HalpAPEntry64:

    /* marker 'w': entered 64-bit long mode (COM1 and VGA) */
    mov     edx, 03F8h
    mov     al, 077h
    out     dx, al
    mov     eax, 0B8F00h
    mov     byte ptr [eax], 057h

    /* Reload the data segments with the flat 64-bit data descriptor so that
     * memory operands below can reach the copied block */
    mov     ax, 020h
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax

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
