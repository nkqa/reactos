/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) long-mode boot entry
 * COPYRIGHT:   Copyright 2024 ReactOS Project
 *              Based on i386 AP entry by Victor Perevertkin and Justin Miller
 *
 * NOTE: Compiled with ml64.exe (MASM64).
 * 16-bit real-mode and 32-bit protected-mode stages are hand-encoded as raw
 * machine bytes (db/dw/dd/dq). The CPU decodes them correctly in each mode.
 *
 * All absolute jump offsets are patched at runtime by HalStartNextProcessor
 * (see spinup.c) to avoid 32-bit ADDR32 relocations in a 64-bit DLL.
 */

PUBLIC HalpAPEntry16
PUBLIC HalpAPEntryData
PUBLIC HalpAPEntry16End
PUBLIC HalpAPEntry32
PUBLIC HalpAPEntry64

OPTION DOTNAME          /* Allow .name local labels */

.code

/*
 * ============================================================
 * 16-bit real-mode stage (raw machine bytes)
 *
 * At APIC SIPI entry: CS = vector << 12, IP = 0
 * Physical base = CS << 4 (identity-mapped < 4GB)
 *
 * Does: cli, flat segs, ebp=phys_base, CR3/CR4/EFER from HAL data,
 *       minimal GDT, enable PM, far-jump to 32-bit code.
 *
 * Offsets from HalpAPEntry16:
 *   +0x30 (abs 0x30): HalpAPEntryData (HAL-patched boot variables)
 *   +0x54: 66 EA (16-bit far jump opcode with 32-bit offset prefix)
 *   +0x58: JumpOffset  (patched by HAL to offset of HalpAPEntry32, CS-relative)
 *   +0x5C: JumpSelector (patched by HAL to 0x08)
 *   +0x60: MinimalGdtDesc (6-byte lgdt descriptor, base patched by HAL)
 *   +0x68: MinimalGdtEntries (6 flat GDT entries)
 *   +0x98: HalpAPEntry32 (32-bit PM stage starts here)
 * ============================================================
 */
HalpAPEntry16:

    /* cli */
    db 0FAh

    /* xor eax, eax + mov ds/es/ss/fs/gs, ax */
    db 066h, 033h, 0C0h
    db 08Eh, 0D8h
    db 08Eh, 0C0h
    db 08Eh, 0D0h
    db 08Eh, 0E0h
    db 08Eh, 0E8h

    /* ebp = CS << 4 (flat physical base) */
    db 066h, 00Fh, 0B7h, 0ECh       /* movzx ebp, cs */
    db 066h, 0C1h, 0E5h, 004h       /* shl ebp, 4 */

    /* Load CR3 = PAE PDPT phys from [ebp + 0x30] */
    db 066h, 067h, 08Bh, 085h, 030h, 000h, 000h, 000h
    db 00Fh, 022h, 0D8h

    /* Load CR4 with PAE from [ebp + 0x38] */
    db 066h, 067h, 08Bh, 085h, 038h, 000h, 000h, 000h
    db 00Fh, 022h, 0E0h

    /* Set EFER.LME + SCE in MSR 0xC0000080 */
    db 0B9h, 080h, 000h, 000h, 0C0h
    db 00Fh, 032h
    db 066h, 067h, 00Bh, 085h, 03Ch, 000h, 000h, 000h
    db 00Fh, 030h

    /* lgdt [ebp + 0x60] - our minimal GDT descriptor */
    db 066h, 067h, 00Fh, 001h, 095h, 060h, 000h, 000h, 000h

    /* Enable PM: CR0.PE = 1 */
    db 00Fh, 020h, 0C0h
    db 083h, 0C8h, 001h
    db 00Fh, 022h, 0C0h

    /* Far-jump to 32-bit PM code (selector 0x08).
     * 16-bit mode with 32-bit offset prefix: 66 EA <off32> <sel16>
     * Offsets are patched by HAL at runtime (see spinup.c). */
    db 066h, 0EAh
HalpAPEntryData:
    /* HAL-patched boot data. Layout MUST match spinup.c AP_ENTRY_DATA_AMD64. */
    /* +0x00 (abs 0x30): PageTableRoot32 (PDPT phys, < 4GB) */
    dq 00000000000000000h
    /* +0x08 (abs 0x38): PaeCr4 */
    dd 000000000h
    /* +0x0C (abs 0x3C): EferFlags (LME + SCE) */
    dd 000000000h
    /* +0x10 (abs 0x40): PageTableRoot64 (kernel 64-bit CR3 low 32 bits) */
    dq 00000000000000000h
    /* +0x18 (abs 0x48): ProcessorState (PKPROCESSOR_STATE virt addr) */
    dq 00000000000000000h
    /* +0x20 (abs 0x50): FlatBase (low stub physical) */
    dq 00000000000000000h

    /* +0x28 (abs 0x58): HalpAPEntry16JumpOffset (patched by HAL: abs addr of HalpAPEntry32 - HalpAPEntry16) */
    dd 000000000h
    /* +0x2C (abs 0x5C): HalpAPEntry16JumpSelector (0x08, patched by HAL) */
    dd 000000000h

    /* +0x30 (abs 0x60): MinimalGdtDesc. 6 bytes: 2-byte limit + 4-byte base.
     * HAL patches base to low stub physical address. */
MinimalGdtDesc:
    dw 0002Fh                       /* limit = 6*8-1 = 47 */
    dd 000000000h                   /* base (HAL patches to low stub phys) */

    /* +0x38 (abs 0x68): MinimalGdtEntries (6 GDT entries, 8 bytes each) */
MinimalGdtEntries:
    dq 00000000000000000h           /* [0] Null */
    dq 00C09A000000FFFFh           /* [1] 32-bit code, base=0, limit=4GB */
    dq 00C092000000FFFFh           /* [2] 32-bit data, base=0, limit=4GB */
    dq 00209A0000000001h           /* [3] 64-bit code, base=0, limit=1, L=1 */
    dq 0000920000000001h           /* [4] 64-bit data, base=0, limit=1 */
    dq 00000000000000000h           /* [5] reserved */

HalpAPEntry16End:

/*
 * ============================================================
 * 32-bit protected-mode stage (raw machine bytes)
 *
 * At entry: CS=0x08, ebp=flat_phys_base, PE=1, PAE=1, CR3=PAE PDPT, EFER.LME=1
 *
 * Does: flat data segs, load kernel CR3, enable paging,
 *       far-jump to 64-bit long-mode code (selector 0x18).
 *
 * Offsets from HalpAPEntry16:
 *   +0x98: 32-bit PM code starts
 *   +0xAB: db 0EAh (far jump opcode)
 *   +0xAC: JumpOffset  (patched by HAL to abs addr of HalpAPEntry64)
 *   +0xB0: JumpSelector (0x18)
 *   +0xB2: HalpAPEntry64 (64-bit code stage starts here)
 * ============================================================
 */
HalpAPEntry32:

    /* Set DS/ES/SS/FS/GS = 0x10 (32-bit flat data) */
    db 0B8h, 010h, 000h, 000h, 000h
    db 08Eh, 0D8h
    db 08Eh, 0C0h
    db 08Eh, 0D0h
    db 08Eh, 0E0h
    db 08Eh, 0E8h

    /* Load kernel CR3 low 32 bits from [ebp + 0x40] */
    db 08Bh, 085h, 040h, 000h, 000h, 000h
    db 00Fh, 022h, 0D8h

    /* Enable paging: CR0.PG = 1 */
    db 00Fh, 020h, 0C0h
    db 00Dh, 000h, 000h, 000h, 080h
    db 00Fh, 022h, 0C0h

    /* Far-jump to 64-bit code (selector 0x18).
     * Offset patched at runtime by HAL (see spinup.c). */
    db 0EAh
    dd 000000000h                   /* offset placeholder - patched by HAL */
    dd 000000018h                   /* selector 0x18 */

/*
 * ============================================================
 * 64-bit long-mode stage (MASM64 native)
 *
 * At entry: CS=0x18, CR0.PG=1 PE=1 PAE=1, CR3=kernel, EFER.LME=1 SCE=1
 *
 * Layout offsets in KPROCESSOR_STATE (from ketypes.h):
 *   SpecialRegisters.Cr0     : +0x00
 *   SpecialRegisters.Cr3     : +0x10
 *   SpecialRegisters.Cr4     : +0x18
 *   SpecialRegisters.Gdtr.Limit : +0x56
 *   SpecialRegisters.Gdtr.Base  : +0x58
 *   SpecialRegisters.Idtr.Limit : +0x66
 *   SpecialRegisters.Idtr.Base  : +0x68
 *   SpecialRegisters.Tr      : +0x70
 *   ContextFrame.Rsp         : 0xE0 + 0x98 = 0x178
 *   ContextFrame.Rip         : 0xE0 + 0xF8 = 0x1D8
 * ============================================================
 */
HalpAPEntry64:

    /* Get address of HalpAPEntryData (rip-relative in MASM64) */
    lea     rax, HalpAPEntryData
    mov     rcx, qword ptr [rax + 18h]     /* ProcessorState at +0x18 */
    test    rcx, rcx
    jz      HaltNow

    mov     r12, rcx                        /* save ProcessorState ptr */

    /* Load CR0 */
    mov     rax, qword ptr [r12 + 00h]
    mov     cr0, rax

    /* Load CR4 */
    mov     rax, qword ptr [r12 + 18h]
    mov     cr4, rax

    /* Load CR3 */
    mov     rax, qword ptr [r12 + 10h]
    mov     cr3, rax

    /* Load GDT. Build 10-byte descriptor on the fly. */
    lea     rdx, HalpFullGdtDesc
    movzx   eax, word ptr [r12 + 56h]       /* Gdtr.Limit */
    mov     word ptr [rdx], ax
    mov     rax, qword ptr [r12 + 58h]      /* Gdtr.Base */
    mov     qword ptr [rdx + 2], rax
    lgdt    fword ptr [rdx]

    /* Load IDT */
    lea     rdx, HalpFullIdtDesc
    movzx   eax, word ptr [r12 + 66h]       /* Idtr.Limit */
    mov     word ptr [rdx], ax
    mov     rax, qword ptr [r12 + 68h]      /* Idtr.Base */
    mov     qword ptr [rdx + 2], rax
    lidt    fword ptr [rdx]

    /* Load TSS selector */
    movzx   eax, word ptr [r12 + 70h]
    ltr     ax

    /* Set GS = KGDT64_R3_DATA | RPL_MASK = 0x2B
     * AMD64 uses MSR_GS_BASE for actual PCR base; selector just needs to non-zero.
     * MSR_GS_BASE must be set by the kernel before GS is used. */
    mov     ax, 0002Bh
    mov     gs, ax

    /* Load kernel stack */
    mov     rsp, qword ptr [r12 + 178h]

    /* Jump to KiSystemStartupBootStack */
    mov     rax, qword ptr [r12 + 1D8h]
    jmp     rax

HaltNow:
    cli
.halt_loop:
    db 0F4h                     /* hlt */
    jmp     .halt_loop

/* 10-byte GDT descriptor buffer for lgdt (2 limit + 8 base) */
HalpFullGdtDesc LABEL FWORD
    dw 00000h
    dq 00000000000000000h

/* 10-byte IDT descriptor buffer for lidt */
HalpFullIdtDesc LABEL FWORD
    dw 00000h
    dq 00000000000000000h

END
