// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "createdump.h"

#if defined(__aarch64__)
// See src/pal/src/include/pal/context.h
#define MCREG_Fp(mc)      ((mc).regs[29])
#define MCREG_Lr(mc)      ((mc).regs[30])
#define MCREG_Sp(mc)      ((mc).sp)
#define MCREG_Pc(mc)      ((mc).pc)
#define MCREG_Cpsr(mc)    ((mc).pstate)
#endif

#ifndef THUMB_CODE
#define THUMB_CODE 1
#endif

#ifndef __GLIBC__
typedef int __ptrace_request;
#endif

bool GetStatus(pid_t pid, pid_t* ppid, pid_t* tgid, std::string* name);

ThreadInfo::ThreadInfo(CrashInfo& crashInfo, pid_t tid) :
    m_crashInfo(crashInfo),
    m_tid(tid),
    m_ppid(0),
    m_tgid(0),
    m_managed(false),
    m_exceptionObject(0),
    m_exceptionHResult(0),
    m_repeatedFrames(0)
{
    m_beginRepeat = m_frames.end();
    m_endRepeat = m_frames.end();
}

ThreadInfo::~ThreadInfo()
{
}

bool
ThreadInfo::Initialize()
{
    if (!GetStatus(m_tid, &m_ppid, &m_tgid, nullptr))
    {
        return false;
    }
    if (!GetRegistersWithPTrace())
    {
        return false;
    }
#if defined(__aarch64__)
    TRACE("Thread %04x PC %016llx SP %016llx\n", m_tid, (unsigned long long)MCREG_Pc(m_gpRegisters), (unsigned long long)MCREG_Sp(m_gpRegisters));
#elif defined(__arm__)
    TRACE("Thread %04x PC %08lx SP %08lx\n", m_tid, (unsigned long)m_gpRegisters.ARM_pc, (unsigned long)m_gpRegisters.ARM_sp);
#elif defined(__x86_64__)
    TRACE("Thread %04x RIP %016llx RSP %016llx\n", m_tid, (unsigned long long)m_gpRegisters.rip, (unsigned long long)m_gpRegisters.rsp);
#elif defined(__loongarch64)
    TRACE("Thread %04x PC %016llx SP %016llx\n", m_tid, (unsigned long long)m_gpRegisters.csr_era, (unsigned long long)m_gpRegisters.regs[3]);
#elif defined(__riscv)
    TRACE("Thread %04x PC %016llx SP %016llx\n", m_tid, (unsigned long long)m_gpRegisters.pc, (unsigned long long)m_gpRegisters.sp);
#else
#error "Unsupported architecture"
#endif
    return true;
}

bool
ThreadInfo::GetRegistersWithPTrace()
{
    struct iovec gpRegsVec = { &m_gpRegisters, sizeof(m_gpRegisters) };
    if (ptrace((__ptrace_request)PTRACE_GETREGSET, m_tid, NT_PRSTATUS, &gpRegsVec) == -1)
    {
        printf_error("ptrace(PTRACE_GETREGSET, %d, NT_PRSTATUS) FAILED %s (%d)\n", m_tid, strerror(errno), errno);
        return false;
    }
    assert(sizeof(m_gpRegisters) == gpRegsVec.iov_len);

    struct iovec fpRegsVec = { &m_fpRegisters, sizeof(m_fpRegisters) };
    if (ptrace((__ptrace_request)PTRACE_GETREGSET, m_tid, NT_FPREGSET, &fpRegsVec) == -1)
    {
#if defined(__arm__)
        // Some aarch64 kernels may not support NT_FPREGSET for arm processes. We treat this failure as non-fatal.
#else
        printf_error("ptrace(PTRACE_GETREGSET, %d, NT_FPREGSET) FAILED %s (%d)\n", m_tid, strerror(errno), errno);
        return false;
#endif
    }
    assert(sizeof(m_fpRegisters) == fpRegsVec.iov_len);

#if defined(__i386__)
    if (ptrace((__ptrace_request)PTRACE_GETFPXREGS, m_tid, nullptr, &m_fpxRegisters) == -1)
    {
        printf_error("ptrace(GETFPXREGS, %d) FAILED %s (%d)\n", m_tid, strerror(errno), errno);
        return false;
    }
#elif defined(__arm__) && defined(__VFP_FP__) && !defined(__SOFTFP__)

#if defined(ARM_VFPREGS_SIZE)
    assert(sizeof(m_vfpRegisters) == ARM_VFPREGS_SIZE);
#endif

    if (ptrace((__ptrace_request)PTRACE_GETVFPREGS, m_tid, nullptr, &m_vfpRegisters) == -1)
    {
        printf_error("ptrace(PTRACE_GETVFPREGS, %d) FAILED %s (%d)\n", m_tid, strerror(errno), errno);
        return false;
    }
#endif
    return true;
}

void
ThreadInfo::GetThreadContext(uint32_t flags, CONTEXT* context) const
{
    context->ContextFlags = flags;
#if defined(__x86_64__)
    if ((flags & CONTEXT_CONTROL) == CONTEXT_CONTROL)
    {
        context->Rbp = m_gpRegisters.rbp;
        context->Rip = m_gpRegisters.rip;
        context->SegCs = m_gpRegisters.cs;
        context->EFlags = m_gpRegisters.eflags;
        context->SegSs = m_gpRegisters.ss;
        context->Rsp = m_gpRegisters.rsp;
    }
    if ((flags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
    {
        context->Rdi = m_gpRegisters.rdi;
        context->Rsi = m_gpRegisters.rsi;
        context->Rbx = m_gpRegisters.rbx;
        context->Rdx = m_gpRegisters.rdx;
        context->Rcx = m_gpRegisters.rcx;
        context->Rax = m_gpRegisters.rax;
        context->R8 = m_gpRegisters.r8;
        context->R9 = m_gpRegisters.r9;
        context->R10 = m_gpRegisters.r10;
        context->R11 = m_gpRegisters.r11;
        context->R12 = m_gpRegisters.r12;
        context->R13 = m_gpRegisters.r13;
        context->R14 = m_gpRegisters.r14;
        context->R15 = m_gpRegisters.r15;
    }
    if ((flags & CONTEXT_SEGMENTS) == CONTEXT_SEGMENTS)
    {
        context->SegDs = m_gpRegisters.ds;
        context->SegEs = m_gpRegisters.es;
        context->SegFs = m_gpRegisters.fs;
        context->SegGs = m_gpRegisters.gs;
    }
    if ((flags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT)
    {
        context->FltSave.ControlWord = m_fpRegisters.cwd;
        context->FltSave.StatusWord = m_fpRegisters.swd;
        context->FltSave.TagWord = m_fpRegisters.ftw;
        context->FltSave.ErrorOpcode = m_fpRegisters.fop;

        context->FltSave.ErrorOffset = FPREG_ErrorOffset(m_fpRegisters);
        context->FltSave.ErrorSelector = FPREG_ErrorSelector(m_fpRegisters);
        context->FltSave.DataOffset = FPREG_DataOffset(m_fpRegisters);
        context->FltSave.DataSelector = FPREG_DataSelector(m_fpRegisters);

        context->FltSave.MxCsr = m_fpRegisters.mxcsr;
        context->FltSave.MxCsr_Mask = m_fpRegisters.mxcr_mask;

        assert(sizeof(context->FltSave.FloatRegisters) == sizeof(m_fpRegisters.st_space));
        memcpy(context->FltSave.FloatRegisters, m_fpRegisters.st_space, sizeof(context->FltSave.FloatRegisters));

        assert(sizeof(context->FltSave.XmmRegisters) == sizeof(m_fpRegisters.xmm_space));
        memcpy(context->FltSave.XmmRegisters, m_fpRegisters.xmm_space, sizeof(context->FltSave.XmmRegisters));
    }
    // TODO: debug registers?
#elif defined(__aarch64__)
    if ((flags & CONTEXT_CONTROL) == CONTEXT_CONTROL)
    {
        context->Fp = MCREG_Fp(m_gpRegisters);
        context->Lr = MCREG_Lr(m_gpRegisters);
        context->Sp = MCREG_Sp(m_gpRegisters);
        context->Pc = MCREG_Pc(m_gpRegisters);
        context->Cpsr = MCREG_Cpsr(m_gpRegisters);
    }
    if ((flags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
    {
        assert(sizeof(m_gpRegisters.regs) == (sizeof(context->X) + sizeof(context->Fp) + sizeof(context->Lr)));
        memcpy(context->X, m_gpRegisters.regs, sizeof(context->X));
    }
    if ((flags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT)
    {
        assert(sizeof(m_fpRegisters.vregs) == sizeof(context->V));
        memcpy(context->V, m_fpRegisters.vregs, sizeof(context->V));
        context->Fpcr = m_fpRegisters.fpcr;
        context->Fpsr = m_fpRegisters.fpsr;
    }
#elif defined(__arm__)
    if ((flags & CONTEXT_CONTROL) == CONTEXT_CONTROL)
    {
        context->Sp = m_gpRegisters.ARM_sp;
        context->Lr = m_gpRegisters.ARM_lr;
        context->Pc = m_gpRegisters.ARM_pc;
        context->Cpsr = m_gpRegisters.ARM_cpsr;
    }
    if ((flags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
    {
        context->R0 = m_gpRegisters.ARM_r0;
        context->R1 = m_gpRegisters.ARM_r1;
        context->R2 = m_gpRegisters.ARM_r2;
        context->R3 = m_gpRegisters.ARM_r3;
        context->R4 = m_gpRegisters.ARM_r4;
        context->R5 = m_gpRegisters.ARM_r5;
        context->R6 = m_gpRegisters.ARM_r6;
        context->R7 = m_gpRegisters.ARM_r7;
        context->R8 = m_gpRegisters.ARM_r8;
        context->R9 = m_gpRegisters.ARM_r9;
        context->R10 = m_gpRegisters.ARM_r10;
        context->R11 = m_gpRegisters.ARM_fp;
        context->R12 = m_gpRegisters.ARM_ip;
    }
    if ((flags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT)
    {
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
        context->Fpscr = m_vfpRegisters.fpscr;

        assert(sizeof(context->D) == sizeof(m_vfpRegisters.fpregs));
        memcpy(context->D, m_vfpRegisters.fpregs, sizeof(context->D));
#endif
    }
#elif defined(__loongarch64)
    if (flags & CONTEXT_CONTROL)
    {
        context->Ra = MCREG_Ra(m_gpRegisters);
        context->Sp = MCREG_Sp(m_gpRegisters);
        context->Fp = MCREG_Fp(m_gpRegisters);
        context->Pc = MCREG_Pc(m_gpRegisters);
    }
    if (flags & CONTEXT_INTEGER)
    {
        memcpy(&context->A0, &m_gpRegisters.regs[4], sizeof(context->A0)*(21 - 4 + 1));
        memcpy(&context->S0, &m_gpRegisters.regs[23], sizeof(context->S0)*9);
    }
    if (flags & CONTEXT_FLOATING_POINT)
    {
        assert(sizeof(context->F) == sizeof(m_fpRegisters.regs));
        memcpy(context->F, m_fpRegisters.regs, sizeof(context->F));
        context->Fcsr = m_fpRegisters.fcsr;
        context->Fcc  = m_fpRegisters.fcc;
    }
#elif defined(__riscv)
    if ((flags & CONTEXT_CONTROL) == CONTEXT_CONTROL)
    {
        context->Ra = MCREG_Ra(m_gpRegisters);
        context->Sp = MCREG_Sp(m_gpRegisters);
        context->Fp = MCREG_Fp(m_gpRegisters);
        context->Pc = MCREG_Pc(m_gpRegisters);
    }

    if (flags & CONTEXT_INTEGER)
    {
        context->Gp = m_gpRegisters.gp;
        context->Tp = m_gpRegisters.tp;
        context->T0 = m_gpRegisters.t0;
        context->T1 = m_gpRegisters.t1;
        context->T2 = m_gpRegisters.t2;
        context->S1 = m_gpRegisters.s1;
        context->A0 = m_gpRegisters.a0;
        context->A1 = m_gpRegisters.a1;
        context->A2 = m_gpRegisters.a2;
        context->A3 = m_gpRegisters.a3;
        context->A4 = m_gpRegisters.a4;
        context->A5 = m_gpRegisters.a5;
        context->A6 = m_gpRegisters.a6;
        context->A7 = m_gpRegisters.a7;
        context->S2 = m_gpRegisters.s2;
        context->S3 = m_gpRegisters.s3;
        context->S4 = m_gpRegisters.s4;
        context->S5 = m_gpRegisters.s5;
        context->S6 = m_gpRegisters.s6;
        context->S7 = m_gpRegisters.s7;
        context->S8 = m_gpRegisters.s8;
        context->S9 = m_gpRegisters.s9;
        context->S10 = m_gpRegisters.s10;
        context->S11 = m_gpRegisters.s11;
        context->T3 = m_gpRegisters.t3;
        context->T4 = m_gpRegisters.t4;
        context->T5 = m_gpRegisters.t5;
        context->T6 = m_gpRegisters.t6;
    }
    if (flags & CONTEXT_FLOATING_POINT)
    {
        assert(sizeof(context->F) == sizeof(m_fpRegisters.fpregs));
        memcpy(context->F, m_fpRegisters.fpregs, sizeof(context->F));
        context->Fcsr = m_fpRegisters.fcsr;
    }
#else
#error Platform not supported
#endif
}
