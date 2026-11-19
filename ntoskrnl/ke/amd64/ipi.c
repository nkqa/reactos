/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IPI code for x64
 * COPYRIGHT:   Copyright 2023 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

#ifdef CONFIG_SMP
#pragma region debug_point_ipi_ack
LONG64 KiIpiGenericCallCount[MAXIMUM_PROCESSORS];
LONG64 KiIpiServiceCount[MAXIMUM_PROCESSORS];
#pragma endregion
#endif

/* FUNCTIONS *****************************************************************/

VOID
FASTCALL
KiIpiSend(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
    /* Check if we can send the IPI directly */
    if (IpiRequest == IPI_APC)
    {
        HalSendSoftwareInterrupt(TargetSet, APC_LEVEL);
    }
    else if (IpiRequest == IPI_DPC)
    {
        HalSendSoftwareInterrupt(TargetSet, DISPATCH_LEVEL);
    }
    else if (IpiRequest == IPI_FREEZE)
    {
        /* On x64 the freeze IPI is an NMI */
        HalSendNMI(TargetSet);
    }
    else
    {
        ASSERT(FALSE);
    }
}

BOOLEAN
NTAPI
KiIpiServiceRoutine(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);

#ifdef CONFIG_SMP
    PKPRCB Prcb = KeGetCurrentPrcb();
    KAFFINITY Senders;
    ULONG Sender;

    ASSERT(KeGetCurrentIrql() == IPI_LEVEL);

    Senders = (KAFFINITY)InterlockedExchange64((PLONG64)&Prcb->SenderSummary, 0);
    while (Senders != 0)
    {
        PREQUEST_MAILBOX Mailbox;
        PKIPI_BROADCAST_WORKER Function;
        ULONG_PTR Argument;
        PLONG Ready;
        PKPRCB SourcePrcb;

        NT_VERIFY(BitScanForwardAffinity(&Sender, Senders));
        Senders &= ~AFFINITY_MASK(Sender);
        Mailbox = &Prcb->RequestMailbox[Sender];
        ASSERT(Mailbox->RequestSummary == IPI_SYNCH_REQUEST);
        Function = (PKIPI_BROADCAST_WORKER)Mailbox->RequestPacket.WorkerRoutine;
        Argument = (ULONG_PTR)Mailbox->RequestPacket.CurrentPacket[0];
        Ready = (PLONG)Mailbox->RequestPacket.CurrentPacket[1];
        SourcePrcb = (PKPRCB)Mailbox->RequestPacket.CurrentPacket[2];

        InterlockedDecrement(Ready);
        while (InterlockedCompareExchange(Ready, 0, 0) != 0)
        {
            YieldProcessor();
        }

        Function(Argument);
#pragma region debug_point_ipi_ack
        InterlockedIncrement64(&KiIpiServiceCount[Prcb->Number]);
#pragma endregion
        InterlockedBitTestAndReset64((PLONG64)&SourcePrcb->TargetSet, Prcb->Number);
    }
#endif
    return TRUE;
}

ULONG_PTR
NTAPI
KeIpiGenericCall(
    _In_ PKIPI_BROADCAST_WORKER Function,
    _In_ ULONG_PTR Argument)
{
    KIRQL OldIrql = KeGetCurrentIrql();
    KIRQL IpiOldIrql;
    ULONG_PTR Result;
#ifdef CONFIG_SMP
    PKPRCB Prcb;
    KAFFINITY TargetSet, RemainingSet;
    ULONG Processor;
    LONG Ready = 1;
#endif

    ASSERT(OldIrql < IPI_LEVEL);
    if (OldIrql < DISPATCH_LEVEL)
    {
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    }

    /* Keep interrupts below IPI_LEVEL while contending for the rendezvous. */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

#ifdef CONFIG_SMP
    Prcb = KeGetCurrentPrcb();
    TargetSet = KeActiveProcessors & ~Prcb->SetMember;
    RemainingSet = TargetSet;
    while (RemainingSet != 0)
    {
        PREQUEST_MAILBOX Mailbox;

        NT_VERIFY(BitScanForwardAffinity(&Processor, RemainingSet));
        RemainingSet &= ~AFFINITY_MASK(Processor);
        Mailbox = &KiProcessorBlock[Processor]->RequestMailbox[Prcb->Number];
        Mailbox->RequestSummary = IPI_SYNCH_REQUEST;
        Mailbox->RequestPacket.WorkerRoutine = (PVOID)Function;
        Mailbox->RequestPacket.CurrentPacket[0] = (PVOID)Argument;
        Mailbox->RequestPacket.CurrentPacket[1] = &Ready;
        Mailbox->RequestPacket.CurrentPacket[2] = Prcb;
        ++Ready;
    }
    InterlockedExchange64((PLONG64)&Prcb->TargetSet, TargetSet);
    RemainingSet = TargetSet;
    while (RemainingSet != 0)
    {
        NT_VERIFY(BitScanForwardAffinity(&Processor, RemainingSet));
        RemainingSet &= ~AFFINITY_MASK(Processor);
        InterlockedOr64((PLONG64)&KiProcessorBlock[Processor]->SenderSummary, Prcb->SetMember);
    }
    if (TargetSet != 0)
    {
        HalRequestIpi(TargetSet);
        while (InterlockedCompareExchange(&Ready, 0, 0) != 1)
        {
            YieldProcessor();
        }
    }
#endif

    KeRaiseIrql(IPI_LEVEL, &IpiOldIrql);
#ifdef CONFIG_SMP
    InterlockedExchange(&Ready, 0);
#endif
    Result = Function(Argument);

#ifdef CONFIG_SMP
#pragma region debug_point_ipi_ack
    InterlockedIncrement64(&KiIpiGenericCallCount[Prcb->Number]);
#pragma endregion
    while (InterlockedCompareExchange64((PLONG64)&Prcb->TargetSet, 0, 0) != 0)
    {
        YieldProcessor();
    }
#endif
    KeLowerIrql(IpiOldIrql);
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);
    KeLowerIrql(OldIrql);
    return Result;
}
