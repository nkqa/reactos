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

// The data necessary for a boot (stored inside HalpLowStub)
extern PVOID HalpAPEntry16;
extern PVOID HalpAPEntryData;
extern PVOID HalpAPEntry16End;
extern PVOID HalpAPEntry32;
extern PVOID HalpAPEntry64;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

ULONG HalpStartedProcessorCount = 1;

#ifndef Add2Ptr
#define Add2Ptr(P,I) ((PVOID)((PUCHAR)(P) + (I)))
#endif
#ifndef PtrOffset
#define PtrOffset(B,O) ((ULONG)((ULONG_PTR)(O) - (ULONG_PTR)(B)))
#endif

/* AMD64 AP entry data structure - matches offsets in apentry.S
 * Size must be <= HALP_LOW_STUB_SIZE_IN_BYTES (16KB).
 */
typedef struct _AP_ENTRY_DATA_AMD64
{
    ULONG64 PageTableRoot32;   /* CR3 for 32-bit PAE stage (phys, < 4GB) */
    ULONG PaeCr4;              /* CR4 with PAE bit set */
    ULONG EferFlags;           /* EFER value with LME + SCE */
    ULONG64 PageTableRoot64;   /* Full kernel 64-bit CR3 (high phys OK) */
    ULONG64 ProcessorState;    /* VA of PKPROCESSOR_STATE (kernel linear) */
    ULONG64 FlatBase;          /* Phys==Linear base of low stub region */
} AP_ENTRY_DATA_AMD64, *PAP_ENTRY_DATA_AMD64;

/* FUNCTIONS *****************************************************************/

static
ULONG64
HalpGetPhysAddr(
    _In_ PVOID Va)
{
    /* HAL can call kernel export directly.
     * Returns physical address as ULONG64.
     */
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(Va);
    return pa.QuadPart;
}

static
ULONG64
HalpSetupTempCr3Amd64(
    VOID)
{
    /*
     * Build a simple PAE page table for the 32-bit AP stage.
     *
     * 32-bit PAE layout: CR3 -> PDPT -> PD -> PT
     *
     * We need to:
     *   1. Identity-map the low stub region (so AP can read boot data)
     *   2. Identity-map the full 4GB physical space (using 2MB huge pages)
     *
     * With 2MB huge pages in PD, we skip PT entirely:
     *   - PDPT has 4 entries (covers 0-4GB virtual)
     *   - Each PDPT entry -> 1 PD (512 PDEs, each 2MB huge page = 1GB per PD)
     *   - Total: 1 PDPT page + 4 PD pages = 5 pages total
     *
     * Allocated at HalpLowStub + 4096 (keep first page for entry code).
     */

    /* Allocate space for PDPT + 4 PDs after the entry code page */
    PVOID Pdp  = Add2Ptr(HalpLowStub, PAGE_SIZE * 1);   /* +4KB */
    PVOID Pd0  = Add2Ptr(HalpLowStub, PAGE_SIZE * 2);   /* +8KB */
    PVOID Pd1  = Add2Ptr(HalpLowStub, PAGE_SIZE * 3);   /* +12KB */
    PVOID Pd2  = Add2Ptr(HalpLowStub, PAGE_SIZE * 4);   /* +16KB */
    PVOID Pd3  = Add2Ptr(HalpLowStub, PAGE_SIZE * 5);   /* +20KB */

    /* Zero everything */
    RtlZeroMemory(Pdp, PAGE_SIZE);
    RtlZeroMemory(Pd0, PAGE_SIZE);
    RtlZeroMemory(Pd1, PAGE_SIZE);
    RtlZeroMemory(Pd2, PAGE_SIZE);
    RtlZeroMemory(Pd3, PAGE_SIZE);

    /* Get physical addresses of our page tables (must be < 4GB for PAE CR3) */
    ULONG64 Pd0Pa = HalpGetPhysAddr(Pd0);
    ULONG64 Pd1Pa = HalpGetPhysAddr(Pd1);
    ULONG64 Pd2Pa = HalpGetPhysAddr(Pd2);
    ULONG64 Pd3Pa = HalpGetPhysAddr(Pd3);

    /* PDPT entries:
     * Format: [phys_page_frame_number][51:12] | [flags:11:0]
     * Flags: P=1, RW=1 (0x03), PS=0 (not huge at PDPT level)
     */
    ((PULONG64)Pdp)[0] = (Pd0Pa << 12) | 0x03ULL;
    ((PULONG64)Pdp)[1] = (Pd1Pa << 12) | 0x03ULL;
    ((PULONG64)Pdp)[2] = (Pd2Pa << 12) | 0x03ULL;
    ((PULONG64)Pdp)[3] = (Pd3Pa << 12) | 0x03ULL;

    /* PD entries with 2MB huge pages:
     * Format: [phys_2mb_page_frame][51:12] | [flags:11:0]
     * Flags: P=1, RW=1, PS=1 (2MB huge page), G=1 = 0x183
     */
    for (ULONG pd_idx = 0; pd_idx < 4; ++pd_idx)
    {
        PVOID Pd = Add2Ptr(HalpLowStub, PAGE_SIZE * (2 + pd_idx));
        PULONG64 Entries = (PULONG64)Pd;
        ULONG64 BasePhy = (ULONG64)pd_idx * 0x40000000ULL; /* Each PD covers 1GB */

        for (ULONG i = 0; i < 512; ++i)
        {
            ULONG64 PhysPage = BasePhy + (ULONG64)i * 0x200000ULL;
            Entries[i] = (PhysPage << 12) | 0x183ULL; /* P + RW + PS + G */
        }
    }

    /* CR3 = Physical address of PDPT (low 12 bits must be 0) */
    ULONG64 PdpPa = HalpGetPhysAddr(Pdp);
    ASSERT((PdpPa & 0xFFF) == 0);       /* Page aligned */
    ASSERT(PdpPa < 0x100000000ULL);     /* < 4GB for 32-bit stage */
    return PdpPa;
}

static
VOID
HalpPatchApEntryAmd64(
    VOID)
{
    /*
     * Patch runtime-computed values into the low stub copy of apentry code.
     *
     * Offsets from HalpAPEntry16 in our low stub copy:
     *   +0x58: 16-bit far-jump offset (32-bit, patched to HalpAPEntry32 phys)
     *   +0x5C: 16-bit far-jump selector (32-bit, patched to 0x00000008)
     *   +0x62: MinimalGdtDesc.base low 32 bits (patched to low stub phys)
     *   +0xAC: 32-bit far-jump offset (32-bit, patched to HalpAPEntry64 phys)
     *
     * The low stub uses identity mapping (phys == linear < 4GB), and
     * flat GDT segments (base == 0). So in protected mode, EIP == phys addr.
     *
     * We use compile-time relative offsets (PtrOffset) to locate each
     * patched field within the low stub copy.
     */
    ULONG64 LowStubPhys = HalpLowStubPhysicalAddress.QuadPart;
    PCHAR LowStub = (PCHAR)HalpLowStub;

    /* 32-bit far jump offset: HalpAPEntry32 label + skip 0EAh opcode */
    ULONG Off32JumpOffset = PtrOffset(HalpAPEntry16, HalpAPEntry32) + 3;

    /* Patch 16-bit far-jump offset: target HalpAPEntry32 phys address */
    ULONG64 Phys32 = LowStubPhys + PtrOffset(HalpAPEntry16, HalpAPEntry32);
    *(PULONG)(LowStub + 0x58) = (ULONG)(Phys32 & 0xFFFFFFFF);

    /* Patch 16-bit far-jump selector: 0x08 (32-bit flat code) */
    *(PULONG)(LowStub + 0x5C) = 0x00000008;

    /* Patch MinimalGdtDesc.base low 32 bits: flat base of our low stub */
    *(PULONG)(LowStub + 0x62) = (ULONG)(LowStubPhys & 0xFFFFFFFF);

    /* Patch 32-bit far-jump offset: target HalpAPEntry64 phys address */
    ULONG64 Phys64 = LowStubPhys + PtrOffset(HalpAPEntry16, HalpAPEntry64);
    *(PULONG)(LowStub + Off32JumpOffset) = (ULONG)(Phys64 & 0xFFFFFFFF);
}

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    ULONG64 TempCr3;
    PAP_ENTRY_DATA_AMD64 ApEntryData;

    /* Bail out if we only use the boot CPU */
    if (HalpOnlyBootProcessor)
        return FALSE;

    /* Bail out if we have started all available CPUs */
    if (HalpStartedProcessorCount >= HalpApicInfoTable.ProcessorCount)
        return FALSE;

    /* Build temporary PAE page table for 32-bit stage */
    TempCr3 = HalpSetupTempCr3Amd64();
    if (!TempCr3)
        return FALSE;

    /* Copy AP entry code into low stub (below 1MB, APIC SIPI requires this) */
    ULONG EntrySize = (ULONG)((ULONG_PTR)HalpAPEntry16End - (ULONG_PTR)HalpAPEntry16);
    ASSERT(EntrySize <= PAGE_SIZE);
    RtlCopyMemory(HalpLowStub, &HalpAPEntry16, EntrySize);

    /* Patch runtime values into the low stub copy */
    HalpPatchApEntryAmd64();

    /* Calculate AP_ENTRY_DATA location inside low stub
     * The AP_ENTRY_DATA is embedded in apentry.S right after HalpAPEntryData label.
     * We find it by computing the same offset.
     */
    ApEntryData = (PAP_ENTRY_DATA_AMD64)Add2Ptr(HalpLowStub,
                   PtrOffset(HalpAPEntry16, HalpAPEntryData));

    /* Fill the AP entry data (matches offsets in apentry.S) */
    ApEntryData->PageTableRoot32 = TempCr3;
    ApEntryData->PaeCr4          = (ULONG)(ProcessorState->SpecialRegisters.Cr4 | 0x00000020ULL); /* CR4.PAE */
    ApEntryData->EferFlags       = 0xC0000080ULL; /* EFER.LME (0x400) + EFER.SCE (0x800) */
    ApEntryData->PageTableRoot64 = ProcessorState->SpecialRegisters.Cr3;
    ApEntryData->ProcessorState  = (ULONG64)(ULONG_PTR)ProcessorState;

    /* Flat base = low stub physical (which equals linear due to identity mapping) */
    ApEntryData->FlatBase = HalpLowStubPhysicalAddress.QuadPart;

    DPRINT("HalStartNextProcessor: CPU %lu, LowStubPhys=%lx, TempCr3=%lx\n",
           HalpStartedProcessorCount,
           HalpLowStubPhysicalAddress.QuadPart,
           TempCr3);

    /* Send INIT + 2x SIPI to start the AP at our low stub address */
    ApicStartApplicationProcessor(HalpStartedProcessorCount, HalpLowStubPhysicalAddress);

    HalpStartedProcessorCount++;
    return TRUE;
}
