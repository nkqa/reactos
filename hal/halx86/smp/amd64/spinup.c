/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) spinup setup
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2024 ReactOS Project
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include <smp.h>
#include <intrin.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern BOOLEAN HalpOnlyBootProcessor;
extern PHYSICAL_ADDRESS HalpLowStubPhysicalAddress;
extern PVOID HalpLowStub;

/* Symbols inside the block copied to HalpLowStub (see apentry.S) */
extern PVOID HalpAPEntry16;
extern PVOID HalpAPEntry16End;
extern PVOID HalpAPJump16;
extern PVOID HalpAPJump32;
extern PVOID HalpAPEntry32;
extern PVOID HalpAPEntry64;
extern PVOID HalpAPEntryData;
extern PVOID HalpMinimalGdtDesc;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* Diagnostics: visible on the boot screen, removed once AP startup is proven */
VOID NTAPI HalDisplayString(PCSTR String);

/* Minimal COM1 output used for early diagnostics (QEMU captures it) */
static
VOID
HalpSerialChar(
    _In_ CHAR Char)
{
    ULONG Spin = 200000;
    while (!(READ_PORT_UCHAR((PUCHAR)0x3FD) & 0x20) && --Spin)
        ;
    WRITE_PORT_UCHAR((PUCHAR)0x3F8, (UCHAR)Char);
}

static
VOID
HalpSerialString(
    _In_ PCSTR String)
{
    while (*String)
        HalpSerialChar(*String++);
}

static
VOID
HalpSerialHex(
    _In_ ULONG64 Value)
{
    static const CHAR Digits[] = "0123456789ABCDEF";
    for (INT i = 60; i >= 0; i -= 4)
        HalpSerialChar(Digits[(Value >> i) & 0xF]);
}

ULONG HalpStartedProcessorCount = 1;

#ifndef Add2Ptr
#define Add2Ptr(P,I) ((PVOID)((PUCHAR)(P) + (I)))
#endif
#ifndef PtrOffset
#define PtrOffset(B,O) ((ULONG)((ULONG_PTR)(O) - (ULONG_PTR)(B)))
#endif

/*
 * Boot variables read by the AP entry code. This layout must match the
 * HalpAPEntryData block in apentry.S exactly.
 *
 * Everything the long mode stage needs is passed as a plain value, so the
 * assembly never has to know the layout of KPROCESSOR_STATE.
 */
typedef struct _AP_ENTRY_DATA_AMD64
{
    ULONG64 PageTableRoot32;   /* Temporary PAE CR3 used while enabling paging */
    ULONG PaeCr4;              /* CR4 value with PAE set */
    ULONG EferFlags;           /* EFER value with LME | SCE */
    ULONG64 PageTableRoot64;   /* Long mode CR3 (temporary PML4) */
    ULONG64 Cr0;               /* CR0 value to restore */
    ULONG64 BootCr3;           /* Boot processor CR3, cloned into the PML4 */
    ULONG64 Pcr;               /* Kernel VA of this processor's PCR */
    ULONG64 KernelStack;       /* Initial RSP for the kernel entry */
    ULONG64 EntryPoint;        /* Initial RIP for the kernel entry */
    ULONG64 FlatBase;          /* Physical base of this block */
    ULONG64 GdtrBase;          /* GDT to load */
    ULONG GdtrLimit;
    ULONG GdtrPad;
    ULONG64 IdtrBase;          /* IDT to load */
    ULONG IdtrLimit;
    ULONG IdtrPad;
    ULONG TssSelector;         /* Task register to load */
    ULONG TssPad;
} AP_ENTRY_DATA_AMD64, *PAP_ENTRY_DATA_AMD64;

C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, PageTableRoot32) == 0x00);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, PaeCr4) == 0x08);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, EferFlags) == 0x0C);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, PageTableRoot64) == 0x10);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, Cr0) == 0x18);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, BootCr3) == 0x20);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, Pcr) == 0x28);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, KernelStack) == 0x30);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, EntryPoint) == 0x38);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, FlatBase) == 0x40);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, GdtrBase) == 0x48);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, GdtrLimit) == 0x50);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, IdtrBase) == 0x58);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, IdtrLimit) == 0x60);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA_AMD64, TssSelector) == 0x68);
C_ASSERT(sizeof(AP_ENTRY_DATA_AMD64) == 0x70);

/* KIPCR.Prcb sits at a fixed offset on AMD64; see sdk/include/ndk/amd64/ketypes.h */
#define HALP_PCR_PRCB_OFFSET 0x180

/* FUNCTIONS *****************************************************************/

static
ULONG64
HalpGetPhysAddr(
    _In_ PVOID Va)
{
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(Va);
    return pa.QuadPart;
}

/*
 * Build the temporary translation tables used while the AP is being brought
 * up. Page 0 of the low stub holds the entry code, pages 1..6 are used here:
 *
 *   page 1 : PML4 (temporary, entry 0 points at the PDPT)
 *   page 2 : PDPT (low 4 GiB)
 *   page 3 : PD0  (identity 0 - 1 GiB, 2 MiB pages)
 *   page 4 : PD1  (identity 1 - 2 GiB)
 *   page 5 : PD2  (identity 2 - 3 GiB)
 *   page 6 : PD3  (identity 3 - 4 GiB)
 *
 * The low 4 GiB are identity mapped with 2 MiB pages so both the 32-bit PAE
 * stage and the long mode stage can keep executing the trampoline, and so the
 * long mode stage can read the boot processor PML4 (which always lives below
 * 4 GiB at this point) in order to clone the kernel address space.
 */
static
BOOLEAN
HalpBuildApPageTables(
    _Out_ PULONG64 PageTableRoot32,
    _Out_ PULONG64 PageTableRoot64)
{
    /* The low stub VA is provided by the ACPI HAL code */
    PVOID StubBase = HalpLowStub;
    if (!StubBase || ((ULONG64)(ULONG_PTR)StubBase < 0xFFFF800000000000ULL))
    {
        HalpSerialString("AP: bad stub VA\r\n");
        return FALSE;
    }

    PVOID Pml4 = Add2Ptr(StubBase, PAGE_SIZE * 1);
    PVOID Pdp  = Add2Ptr(StubBase, PAGE_SIZE * 2);

    HalpSerialString("AP: Stub=");
    HalpSerialHex((ULONG64)(ULONG_PTR)StubBase);
    HalpSerialString(" Phys=");
    HalpSerialHex(HalpLowStubPhysicalAddress.QuadPart);
    HalpSerialString("\r\n");

    RtlZeroMemory(Pml4, PAGE_SIZE);
    RtlZeroMemory(Pdp, PAGE_SIZE);

    ULONG64 Pml4Pa = HalpGetPhysAddr(Pml4);
    ULONG64 PdpPa  = HalpGetPhysAddr(Pdp);

    HalpSerialString("AP: Pml4Pa=");
    HalpSerialHex(Pml4Pa);
    HalpSerialString(" PdpPa=");
    HalpSerialHex(PdpPa);
    HalpSerialString("\r\n");

    /* The 32-bit stage can only load the low 32 bits of CR3 */
    if ((Pml4Pa | PdpPa) >> 32)
    {
        DPRINT1("HalpBuildApPageTables: low stub above 4 GiB\n");
        return FALSE;
    }

    /* A single page directory covers the low 1 GiB identity mapping, which is
     * enough for the trampoline, the page tables and the boot PML4 itself. */
    for (ULONG Index = 0; Index < 1; Index++)
    {
        PVOID Pd = Add2Ptr(StubBase, PAGE_SIZE * (3 + Index));
        RtlZeroMemory(Pd, PAGE_SIZE);

        ULONG64 PdPa = HalpGetPhysAddr(Pd);
        if (PdPa >> 32)
        {
            DPRINT1("HalpBuildApPageTables: page directory above 4 GiB\n");
            return FALSE;
        }

        /* PDPT entry -> page directory */
        ((PULONG64)Pdp)[Index] = (PdPa & ~0xFFFULL) | 0x03ULL;

        /* 2 MiB identity pages: P + RW + PS + G */
        ULONG64 Base = (ULONG64)Index * 0x40000000ULL;
        for (ULONG i = 0; i < 512; i++)
        {
            ((PULONG64)Pd)[i] = (Base + ((ULONG64)i * 0x200000ULL)) | 0x183ULL;
        }
    }

    /* PML4 entry 0 wraps the PDPT, giving the low 4 GiB identity mapping.
     * Entries 1..511 are cloned from the boot processor by the AP itself,
     * which avoids mapping page tables this early during Phase 0. */
    ((PULONG64)Pml4)[0] = (PdpPa & ~0xFFFULL) | 0x03ULL;

    /* The AP reads the boot PML4 through the identity mapping, so it must be
     * reachable from the 32-bit address space. */
    ULONG64 BootCr3 = __readcr3() & ~0xFFFULL;
    if (BootCr3 >> 32)
    {
        DPRINT1("HalpBuildApPageTables: boot PML4 above 4 GiB (0x%I64x)\n", BootCr3);
        return FALSE;
    }

    *PageTableRoot32 = PdpPa;
    *PageTableRoot64 = Pml4Pa;
    return TRUE;
}

/*
 * Fill in the runtime values of the copied block. All offsets are derived from
 * exported labels instead of being hardcoded, so the assembly layout can
 * change without silently corrupting the trampoline.
 */
static
VOID
HalpPatchApEntryAmd64(
    _In_ ULONG64 PageTableRoot32,
    _In_ ULONG64 PageTableRoot64,
    _In_ ULONG PaeCr4,
    _In_ ULONG EferFlags,
    _In_ PKPROCESSOR_STATE ProcessorState,
    _In_ ULONG64 Pcr)
{
    ULONG64 LowStubPhys = HalpLowStubPhysicalAddress.QuadPart;
    PCHAR Base = (PCHAR)HalpLowStub;

    ULONG OffJump16 = PtrOffset(&HalpAPEntry16, &HalpAPJump16);
    ULONG OffJump32 = PtrOffset(&HalpAPEntry16, &HalpAPJump32);
    ULONG OffGdtDesc = PtrOffset(&HalpAPEntry16, &HalpMinimalGdtDesc);
    ULONG OffData = PtrOffset(&HalpAPEntry16, &HalpAPEntryData);

    /* Absolute physical targets of the 32-bit and 64-bit stages */
    ULONG64 Phys32 = LowStubPhys + PtrOffset(&HalpAPEntry16, &HalpAPEntry32);
    ULONG64 Phys64 = LowStubPhys + PtrOffset(&HalpAPEntry16, &HalpAPEntry64);

    ASSERT((Phys32 | Phys64) >> 32 == 0);

    /* 16-bit far jump: offset32 + selector16 0x08 */
    *(PULONG)(Base + OffJump16) = (ULONG)Phys32;
    *(PUSHORT)(Base + OffJump16 + 4) = 0x0008;

    /* 32-bit far jump: offset32 + selector16 0x18 */
    *(PULONG)(Base + OffJump32) = (ULONG)Phys64;
    *(PUSHORT)(Base + OffJump32 + 4) = 0x0018;

    /* Minimal GDT base (16-bit lgdt takes a 32-bit base) */
    *(PULONG)(Base + OffGdtDesc + 2) = (ULONG)LowStubPhys;

    /* Boot variables */
    PAP_ENTRY_DATA_AMD64 Data = (PAP_ENTRY_DATA_AMD64)(Base + OffData);
    Data->PageTableRoot32 = PageTableRoot32;
    Data->PaeCr4 = PaeCr4;
    Data->EferFlags = EferFlags;
    Data->PageTableRoot64 = PageTableRoot64;
    Data->Cr0 = ProcessorState->SpecialRegisters.Cr0;
    Data->BootCr3 = ProcessorState->SpecialRegisters.Cr3;
    Data->Pcr = Pcr;
    Data->KernelStack = ProcessorState->ContextFrame.Rsp;
    Data->EntryPoint = ProcessorState->ContextFrame.Rip;
    Data->FlatBase = LowStubPhys;
    Data->GdtrBase = (ULONG64)(ULONG_PTR)ProcessorState->SpecialRegisters.Gdtr.Base;
    Data->GdtrLimit = ProcessorState->SpecialRegisters.Gdtr.Limit;
    Data->IdtrBase = (ULONG64)(ULONG_PTR)ProcessorState->SpecialRegisters.Idtr.Base;
    Data->IdtrLimit = ProcessorState->SpecialRegisters.Idtr.Limit;
    Data->TssSelector = ProcessorState->SpecialRegisters.Tr;
}

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    ULONG64 PageTableRoot32, PageTableRoot64;
    ULONG64 Pcr;

    /* Only the boot processor requested */
    if (HalpOnlyBootProcessor)
        return FALSE;

    /* All detected processors already started */
    if (HalpStartedProcessorCount >= HalpApicInfoTable.ProcessorCount)
        return FALSE;

    HalDisplayString("AP: entered\n");

    if (!HalpLowStubPhysicalAddress.QuadPart)
    {
        HalDisplayString("AP: no low stub\n");
        return FALSE;
    }

    /* Build the temporary identity mapping */
    if (!HalpBuildApPageTables(&PageTableRoot32, &PageTableRoot64))
    {
        HalDisplayString("AP: page tables failed\n");
        return FALSE;
    }
    HalDisplayString("AP: page tables\n");

    /* Copy the trampoline below 1 MiB, where the SIPI vector can reach it */
    ULONG EntrySize = (ULONG)((ULONG_PTR)&HalpAPEntry16End - (ULONG_PTR)&HalpAPEntry16);
    ASSERT(EntrySize <= PAGE_SIZE);
    if (EntrySize > PAGE_SIZE)
    {
        HalDisplayString("AP: entry too large\n");
        return FALSE;
    }
    RtlCopyMemory(HalpLowStub, &HalpAPEntry16, EntrySize);
    HalDisplayString("AP: entry copied\n");

    /* LoaderBlock->Prcb points at KIPCR.Prcb of the new processor */
    Pcr = (ULONG64)(ULONG_PTR)LoaderBlock->Prcb - HALP_PCR_PRCB_OFFSET;

    DPRINT("HalStartNextProcessor: CPU %lu, LowStub=%lx, PML4=%lx, PCR=%lx\n",
           HalpStartedProcessorCount,
           HalpLowStubPhysicalAddress.QuadPart,
           PageTableRoot64,
           Pcr);

    /* Patch the copied block with this processor's specific values.
     * EFER.LME (0x400) puts the CPU into long mode, EFER.SCE (0x800) enables
     * the syscall/sysret instructions. */
    HalpPatchApEntryAmd64(PageTableRoot32,
                          PageTableRoot64,
                          (ULONG)(ProcessorState->SpecialRegisters.Cr4 | CR4_PAE),
                          (ULONG)(0x400 | 0x800),
                          ProcessorState,
                          Pcr);
    HalDisplayString("AP: patched, sending SIPI\n");

    HalpSerialString("AP: count=");
    HalpSerialHex(HalpApicInfoTable.ProcessorCount);
    HalpSerialString(" idx=");
    HalpSerialHex(HalpStartedProcessorCount);
    HalpSerialString(" lapic=");
    HalpSerialHex(HalpProcessorIdentity[HalpStartedProcessorCount].LapicId);
    HalpSerialString(" start=");
    HalpSerialHex(HalpLowStubPhysicalAddress.QuadPart);
    HalpSerialString("\r\n");

    /* Send INIT + two SIPIs */
    ApicStartApplicationProcessor(HalpStartedProcessorCount, HalpLowStubPhysicalAddress);
    HalDisplayString("AP: SIPI sent\n");

    HalpStartedProcessorCount++;
    return TRUE;
}
