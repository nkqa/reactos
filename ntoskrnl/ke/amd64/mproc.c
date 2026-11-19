/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2024 ReactOS Project
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* AMD64 ketypes.h is missing these GDT selector definitions that exist on i386.
 * They follow the same numeric values since GDT layout is shared. */
#ifndef KGDT_R0_PCR
#define KGDT_R0_PCR             0x0030
#endif
#ifndef KGDT64_DF_TSS
#define KGDT64_DF_TSS           0x0050
#endif
#ifndef KGDT64_NMI_TSS
#define KGDT64_NMI_TSS          0x0058
#endif

/* Forward declarations for functions not in headers */
CODE_SEG("INIT") VOID NTAPI KiInitializeProcessorBootStructures(
    ULONG ProcessorNumber,
    PKIPCR Pcr,
    PKGDTENTRY64 GdtBase,
    PKIDTENTRY64 IdtBase,
    PKTSS64 TssBase,
    PKTHREAD IdleThread,
    PVOID KernelStack,
    PVOID DpcStack,
    PVOID DoubleFaultStack,
    PVOID NmiStack);

CODE_SEG("INIT") DECLSPEC_NORETURN VOID NTAPI KiSystemStartupBootStack(VOID);

/*
 * APINFO - Per-AP bootstrap context
 * Allocated once per AP on the boot CPU. The AP uses this to initialize
 * its own PCR, GDT/IDT/TSS, and thread context before jumping into
 * KiSystemStartupBootStack.
 */
typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY64 Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY64 Gdt[128];
    DECLSPEC_ALIGN(16) UINT8 NMIStackData[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS Tss;
    KTSS TssDoubleFault;
    KTSS TssNMI;
} APINFO, *PAPINFO;

/* The HAL derives the PCR address from LoaderBlock->Prcb by subtracting this
 * offset, keep the two in sync. */
C_ASSERT(FIELD_OFFSET(KIPCR, Prcb) == 0x180);

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack, DPCStack;
    PAPINFO APInfo;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;
    ULONG Timeout;

    /* Determine maximum number of processors we can start */
    MaximumProcessors = KeMaximumProcessors;
    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);
    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    DPRINT("KeStartAllProcessors: Starting APs up to %lu\n", MaximumProcessors);

    /* Start AP count at 1 because CPU 0 is the boot processor */
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        KernelStack = NULL;
        DPCStack = NULL;
        APInfo = NULL;

        /* Allocate per-AP bootstrap structures */
        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (!APInfo)
        {
            DPRINT1("KeStartAllProcessors: Cannot allocate APInfo for CPU %lu\n", ProcessorCount);
            break;
        }
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);

        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (!KernelStack)
        {
            DPRINT1("KeStartAllProcessors: Cannot create kernel stack for CPU %lu\n", ProcessorCount);
            break;
        }

        DPCStack = MmCreateKernelStack(FALSE, 0);
        if (!DPCStack)
        {
            DPRINT1("KeStartAllProcessors: Cannot create DPC stack for CPU %lu\n", ProcessorCount);
            break;
        }

        /* Initialize the new PCR for this AP */
        KiInitializeProcessorBootStructures(ProcessorCount,
                                            &APInfo->Pcr,
                                            &APInfo->Gdt[0],
                                            &APInfo->Idt[0],
                                            &APInfo->Tss,
                                            (PKTHREAD)&APInfo->Thread,
                                            KernelStack,
                                            DPCStack,
                                            APInfo->NMIStackData,
                                            APInfo->NMIStackData);

        /* Clone BSP's GDT and IDT into our per-AP copies */
        KDESCRIPTOR BspGdt, BspIdt;
        __sgdt(&BspGdt.Limit);
        __sidt(&BspIdt.Limit);
        RtlCopyMemory(&APInfo->Gdt[0], (PVOID)BspGdt.Base, BspGdt.Limit + 1);
        RtlCopyMemory(&APInfo->Idt[0], (PVOID)BspIdt.Base, BspIdt.Limit + 1);

        /* Patch GDT entries to point to this AP's PCR and TSS */
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt[0], KGDT64_SYS_TSS),
                               (ULONG_PTR)&APInfo->Tss);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt[0], KGDT_R0_PCR),
                               (ULONG_PTR)&APInfo->Pcr);

        /* Double-fault and NMI TSS entries */
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt[0], KGDT64_DF_TSS),
                               (ULONG_PTR)&APInfo->TssDoubleFault);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt[0], KGDT64_NMI_TSS),
                               (ULONG_PTR)&APInfo->TssNMI);

        /* Initialize TSS stack pointers */
        APInfo->Tss.Rsp0 = (ULONG64)KernelStack;
        APInfo->Tss.Rsp1 = (ULONG64)DPCStack;
        APInfo->Tss.Ist[2] = (ULONG64)APInfo->NMIStackData;

        /* Double-fault and NMI TSS stacks */
        APInfo->TssDoubleFault.Rsp0 = (ULONG64)APInfo->NMIStackData;
        APInfo->TssNMI.Rsp0 = (ULONG64)APInfo->NMIStackData;

        /* Fill in the AP's PKPROCESSOR_STATE.
         * NOTE: KPRCB is a VALUE member of KIPCR, not a pointer! */
        PKPROCESSOR_STATE ProcessorState = &APInfo->Pcr.Prcb.ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        /* Save current control registers from BSP */
        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr2 = __readcr2();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();

        /* Segment registers for the new AP */
        ProcessorState->ContextFrame.SegCs = KGDT64_R0_CODE;
        ProcessorState->ContextFrame.SegDs = KGDT64_R3_DATA;
        ProcessorState->ContextFrame.SegEs = KGDT64_R3_DATA;
        ProcessorState->ContextFrame.SegSs = KGDT64_R0_DATA;
        ProcessorState->ContextFrame.SegFs = KGDT_R0_PCR;
        ProcessorState->ContextFrame.SegGs = KGDT_R0_PCR;

        /* Full 64-bit GDT/IDT descriptors (Base is PVOID in KDESCRIPTOR) */
        ProcessorState->SpecialRegisters.Gdtr.Base = (PVOID)&APInfo->Gdt[0];
        ProcessorState->SpecialRegisters.Gdtr.Limit = sizeof(APInfo->Gdt) - 1;
        ProcessorState->SpecialRegisters.Idtr.Base = (PVOID)&APInfo->Idt[0];
        ProcessorState->SpecialRegisters.Idtr.Limit = sizeof(APInfo->Idt) - 1;

        /* TSS selector pointing into our per-AP GDT */
        ProcessorState->SpecialRegisters.Tr = KGDT64_SYS_TSS;

        /* Set up the startup stack:
         * AP entry loads Rsp and RIp from ProcessorState and jumps.
         * KiSystemStartupBootStack is the kernel's AP startup routine.
         */
        ProcessorState->ContextFrame.Rsp = (ULONG64)KernelStack;
        ProcessorState->ContextFrame.Rip = (ULONG64)KiSystemStartupBootStack;
        ProcessorState->ContextFrame.EFlags = __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        /* Update LoaderBlock for this AP.
         * KiSystemStartupBootStack dereferences Thread->ApcState.Process before
         * the thread is initialized, so point it at the system process here.
         * All LoaderBlock fields are ULONG_PTR. */
        APInfo->Thread.Tcb.ApcState.Process = &PsInitialSystemProcess->Pcb;
        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)&APInfo->Pcr.Prcb;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread.Tcb;
        KeLoaderBlock->Process = (ULONG_PTR)&PsInitialSystemProcess->Pcb;

        /* Start this AP via the HAL */
        DPRINT("KeStartAllProcessors: Attempting to start CPU %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            DPRINT1("KeStartAllProcessors: HalStartNextProcessor failed for CPU %lu\n", ProcessorCount);
            goto StartFailed;
        }

        /* Wait for the AP to come up. The AP clears KeLoaderBlock->Prcb once it
         * has finished its own boot setup. Poll for up to five seconds. */
        Timeout = 5000;
        while (KeLoaderBlock->Prcb != 0)
        {
            KeMemoryBarrier();
            KeStallExecutionProcessor(1000);
            if (--Timeout == 0)
                break;
        }

        if (KeLoaderBlock->Prcb != 0)
        {
            DPRINT1("KeStartAllProcessors: CPU %lu did not start\n", ProcessorCount);
            goto StartFailed;
        }

        /* The processor is running and owns these structures now, never free
         * them: the PCR, GDT, TSS and stacks are still in use. */
        continue;

StartFailed:
        ExFreePoolWithTag(APInfo, TAG_KERNEL);
        MmDeleteKernelStack(KernelStack, FALSE);
        MmDeleteKernelStack(DPCStack, FALSE);
        break;
    }

    DPRINT1("KeStartAllProcessors: %lu AP(s) started successfully\n", ProcessorCount);
}
