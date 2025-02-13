#include "Interface/Core/Dispatcher/Dispatcher.h"

#include "Common/MathUtils.h"
#include "Interface/Core/ArchHelpers/MContext.h"
#include <FEXCore/Core/X86Enums.h>

namespace FEXCore::CPU {

void Dispatcher::SleepThread(FEXCore::Context::Context *ctx, FEXCore::Core::CpuStateFrame *Frame) {
  auto Thread = Frame->Thread;

  --ctx->IdleWaitRefCount;
  ctx->IdleWaitCV.notify_all();

  // Go to sleep
  Thread->StartRunning.Wait();

  Thread->RunningEvents.Running = true;
  ++ctx->IdleWaitRefCount;
  ctx->IdleWaitCV.notify_all();
}

void Dispatcher::StoreThreadState(int Signal, void *ucontext) {
  // We can end up getting a signal at any point in our host state
  // Jump to a handler that saves all state so we can safely return
  uint64_t OldSP = ArchHelpers::Context::GetSp(ucontext);
  uintptr_t NewSP = OldSP;

  size_t StackOffset = sizeof(ArchHelpers::Context::ContextBackup);

  // We need to back up behind the host's red zone
  // We do this on the guest side as well
  // (does nothing on arm hosts)
  NewSP -= ArchHelpers::Context::ContextBackup::RedZoneSize;

  NewSP -= StackOffset;
  NewSP = AlignDown(NewSP, 16);

  auto Context = reinterpret_cast<ArchHelpers::Context::ContextBackup*>(NewSP);
  ArchHelpers::Context::BackupContext(ucontext, Context);

  // Retain the action pointer so we can see it when we return
  Context->Signal = Signal;

  // Save guest state
  // We can't guarantee if registers are in context or host GPRs
  // So we need to save everything
  memcpy(&Context->GuestState, ThreadState->CurrentFrame, sizeof(FEXCore::Core::CPUState));

  // Set the new SP
  ArchHelpers::Context::SetSp(ucontext, NewSP);

  SignalFrames.push(NewSP);
}

void Dispatcher::RestoreThreadState(void *ucontext) {
  LOGMAN_THROW_A(!SignalFrames.empty(), "Trying to restore a signal frame when we don't have any");
  uint64_t OldSP = SignalFrames.top();
  SignalFrames.pop();
  uintptr_t NewSP = OldSP;
  auto Context = reinterpret_cast<ArchHelpers::Context::ContextBackup*>(NewSP);

  // First thing, reset the guest state
  memcpy(ThreadState->CurrentFrame, &Context->GuestState, sizeof(FEXCore::Core::CPUState));

  // Now restore host state
  ArchHelpers::Context::RestoreContext(ucontext, Context);

  // Restore the previous signal state
  // This allows recursive signals to properly handle signal masking as we are walking back up the list of signals
  CTX->SignalDelegation->SetCurrentSignal(Context->Signal);
}

bool Dispatcher::HandleGuestSignal(int Signal, void *info, void *ucontext, GuestSigAction *GuestAction, stack_t *GuestStack) {
  StoreThreadState(Signal, ucontext);
  auto Frame = ThreadState->CurrentFrame;

  // Ref count our faults
  // We use this to track if it is safe to clear cache
  ++SignalHandlerRefCounter;

  // Set the new PC
  ArchHelpers::Context::SetPc(ucontext, AbsoluteLoopTopAddressFillSRA);
  // Set our state register to point to our guest thread data
  ArchHelpers::Context::SetState(ucontext, reinterpret_cast<uint64_t>(Frame));


  uint64_t OldGuestSP = Frame->State.gregs[X86State::REG_RSP];
  uint64_t NewGuestSP = OldGuestSP;

  if (!(GuestStack->ss_flags & SS_DISABLE)) {
    // If our guest is already inside of the alternative stack
    // Then that means we are hitting recursive signals and we need to walk back the stack correctly
    uint64_t AltStackBase = reinterpret_cast<uint64_t>(GuestStack->ss_sp);
    uint64_t AltStackEnd = AltStackBase + GuestStack->ss_size;
    if (OldGuestSP >= AltStackBase &&
        OldGuestSP <= AltStackEnd) {
      // We are already in the alt stack, the rest of the code will handle adjusting this
    }
    else {
      NewGuestSP = AltStackEnd;
    }
  }

  // Back up past the redzone, which is 128bytes
  // Don't need this offset if we aren't going to be putting siginfo in to it
  NewGuestSP -= 128;

  // siginfo_t
  siginfo_t *HostSigInfo = reinterpret_cast<siginfo_t*>(info);

  if (GuestAction->sa_flags & SA_SIGINFO &&
      !(HostSigInfo->si_code == SI_QUEUE || // If the siginfo comes from sigqueue or user then we don't need to check
        HostSigInfo->si_code == SI_USER)) {
    if (SRAEnabled) {
      if (!IsAddressInJITCode(ArchHelpers::Context::GetPc(ucontext), false)) {
        LOGMAN_THROW_A(!IsAddressInJITCode(ArchHelpers::Context::GetPc(ucontext), true), "Signals in dispatcher have unsynchronized context");
      } else {
        // We are in jit, SRA must be spilled
        SpillSRA(ucontext);
      }
    }

    // Setup ucontext a bit
    if (CTX->Config.Is64BitMode) {
      NewGuestSP -= sizeof(FEXCore::x86_64::ucontext_t);
      uint64_t UContextLocation = NewGuestSP;

      NewGuestSP -= sizeof(siginfo_t);
      uint64_t SigInfoLocation = NewGuestSP;

      FEXCore::x86_64::ucontext_t *guest_uctx = reinterpret_cast<FEXCore::x86_64::ucontext_t*>(UContextLocation);
      siginfo_t *guest_siginfo = reinterpret_cast<siginfo_t*>(SigInfoLocation);

      // We have extended float information
      guest_uctx->uc_flags |= FEXCore::x86_64::UC_FP_XSTATE;

      // Pointer to where the fpreg memory is
      guest_uctx->uc_mcontext.fpregs = &guest_uctx->__fpregs_mem;

      guest_uctx->uc_mcontext.gregs[FEXCore::x86_64::FEX_REG_RIP] = Frame->State.rip;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86_64::FEX_REG_EFL] = 0;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86_64::FEX_REG_CSGSFS] = 0;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86_64::FEX_REG_ERR] = 0;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86_64::FEX_REG_TRAPNO] = Signal;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86_64::FEX_REG_OLDMASK] = 0;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86_64::FEX_REG_CR2] = 0;

#define COPY_REG(x) \
      guest_uctx->uc_mcontext.gregs[FEXCore::x86_64::FEX_REG_##x] = Frame->State.gregs[X86State::REG_##x];
      COPY_REG(R8);
      COPY_REG(R9);
      COPY_REG(R10);
      COPY_REG(R11);
      COPY_REG(R12);
      COPY_REG(R13);
      COPY_REG(R14);
      COPY_REG(R15);
      COPY_REG(RDI);
      COPY_REG(RSI);
      COPY_REG(RBP);
      COPY_REG(RBX);
      COPY_REG(RDX);
      COPY_REG(RAX);
      COPY_REG(RCX);
      COPY_REG(RSP);
#undef COPY_REG

      // Copy float registers
      memcpy(guest_uctx->__fpregs_mem._st, Frame->State.mm, sizeof(Frame->State.mm));
      memcpy(guest_uctx->__fpregs_mem._xmm, Frame->State.xmm, sizeof(Frame->State.xmm));

      // FCW store default
      guest_uctx->__fpregs_mem.fcw = Frame->State.FCW;
      guest_uctx->__fpregs_mem.ftw = Frame->State.FTW;

      // Reconstruct FSW
      guest_uctx->__fpregs_mem.fsw =
        (Frame->State.flags[FEXCore::X86State::X87FLAG_TOP_LOC] << 11) |
        (Frame->State.flags[FEXCore::X86State::X87FLAG_C0_LOC] << 8) |
        (Frame->State.flags[FEXCore::X86State::X87FLAG_C1_LOC] << 9) |
        (Frame->State.flags[FEXCore::X86State::X87FLAG_C2_LOC] << 10) |
        (Frame->State.flags[FEXCore::X86State::X87FLAG_C3_LOC] << 14);

      // Copy over signal stack information
      guest_uctx->uc_stack.ss_flags = GuestStack->ss_flags;
      guest_uctx->uc_stack.ss_sp = GuestStack->ss_sp;
      guest_uctx->uc_stack.ss_size = GuestStack->ss_size;

      // aarch64 and x86_64 siginfo_t matches. We can just copy this over
      // SI_USER could also potentially have random data in it, needs to be bit perfect
      // For guest faults we don't have a real way to reconstruct state to a real guest RIP
      *guest_siginfo = *HostSigInfo;

      Frame->State.gregs[X86State::REG_RSI] = SigInfoLocation;
      Frame->State.gregs[X86State::REG_RDX] = UContextLocation;
    }
    else {
      // XXX: 32bit Support
      NewGuestSP -= sizeof(FEXCore::x86::ucontext_t);
      uint64_t UContextLocation = NewGuestSP;
      NewGuestSP -= sizeof(FEXCore::x86::siginfo_t);
      uint64_t SigInfoLocation = NewGuestSP;

      FEXCore::x86::ucontext_t *guest_uctx = reinterpret_cast<FEXCore::x86::ucontext_t*>(UContextLocation);
      FEXCore::x86::siginfo_t *guest_siginfo = reinterpret_cast<FEXCore::x86::siginfo_t*>(SigInfoLocation);

      // We have extended float information
      guest_uctx->uc_flags |= FEXCore::x86::UC_FP_XSTATE;

      // Pointer to where the fpreg memory is
      guest_uctx->uc_mcontext.fpregs = static_cast<uint32_t>(reinterpret_cast<uint64_t>(&guest_uctx->__fpregs_mem));

      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_GS] = Frame->State.gs;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_FS] = Frame->State.fs;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_ES] = Frame->State.es;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_DS] = Frame->State.ds;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_TRAPNO] = Signal;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_ERR] = 0;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_EIP] = Frame->State.rip;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_CS] = Frame->State.cs;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_EFL] = 0;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_UESP] = 0;
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_SS] = Frame->State.ss;

#define COPY_REG(x) \
      guest_uctx->uc_mcontext.gregs[FEXCore::x86::FEX_REG_##x] = Frame->State.gregs[X86State::REG_##x];
      COPY_REG(RDI);
      COPY_REG(RSI);
      COPY_REG(RBP);
      COPY_REG(RBX);
      COPY_REG(RDX);
      COPY_REG(RAX);
      COPY_REG(RCX);
      COPY_REG(RSP);
#undef COPY_REG

      // Copy float registers
      memcpy(guest_uctx->__fpregs_mem._st, Frame->State.mm, sizeof(Frame->State.mm));
      if (0) {
        // XXX: Handle XMM
        // memcpy(guest_uctx->__fpregs_mem._xmm, Frame->State.xmm, sizeof(Frame->State.xmm));
        guest_uctx->__fpregs_mem.status = FEXCore::x86::fpstate_magic::MAGIC_XFPSTATE;
      }
      else {
        guest_uctx->__fpregs_mem.status = FEXCore::x86::fpstate_magic::MAGIC_FPU;
      }
      // FCW store default
      guest_uctx->__fpregs_mem.fcw = Frame->State.FCW;
      guest_uctx->__fpregs_mem.ftw = Frame->State.FTW;
      // Reconstruct FSW
      guest_uctx->__fpregs_mem.fsw =
        (Frame->State.flags[FEXCore::X86State::X87FLAG_TOP_LOC] << 11) |
        (Frame->State.flags[FEXCore::X86State::X87FLAG_C0_LOC] << 8) |
        (Frame->State.flags[FEXCore::X86State::X87FLAG_C1_LOC] << 9) |
        (Frame->State.flags[FEXCore::X86State::X87FLAG_C2_LOC] << 10) |
        (Frame->State.flags[FEXCore::X86State::X87FLAG_C3_LOC] << 14);

      // Copy over signal stack information
      guest_uctx->uc_stack.ss_flags = GuestStack->ss_flags;
      guest_uctx->uc_stack.ss_sp = static_cast<uint32_t>(reinterpret_cast<uint64_t>(GuestStack->ss_sp));
      guest_uctx->uc_stack.ss_size = GuestStack->ss_size;

      // These three elements are in every siginfo
      guest_siginfo->si_signo = HostSigInfo->si_signo;
      guest_siginfo->si_errno = HostSigInfo->si_errno;
      guest_siginfo->si_code = HostSigInfo->si_code;

      switch (Signal) {
        case SIGSEGV:
        case SIGBUS:
          // Macro expansion to get the si_addr
          // Can't really give a real result here. Pull from the context for now
          guest_siginfo->_sifields._sigfault.addr = Frame->State.rip;
          break;
        case SIGCHLD:
          guest_siginfo->_sifields._sigchld.pid = HostSigInfo->si_pid;
          guest_siginfo->_sifields._sigchld.uid = HostSigInfo->si_uid;
          guest_siginfo->_sifields._sigchld.status = HostSigInfo->si_status;
          guest_siginfo->_sifields._sigchld.utime = HostSigInfo->si_utime;
          guest_siginfo->_sifields._sigchld.stime = HostSigInfo->si_stime;
          break;
      default:
        // Hope for the best, most things just copy over
        memcpy(&guest_siginfo->_sifields, &HostSigInfo->_sifields, sizeof(siginfo_t));
        break;
      }

      NewGuestSP -= 4;
      *(uint32_t*)NewGuestSP = UContextLocation;
      NewGuestSP -= 4;
      *(uint32_t*)NewGuestSP = SigInfoLocation;
      NewGuestSP -= 4;
      *(uint32_t*)NewGuestSP = Signal;
    }

    Frame->State.rip = reinterpret_cast<uint64_t>(GuestAction->sigaction_handler.sigaction);
  }
  else {
    if (!CTX->Config.Is64BitMode) {
      NewGuestSP -= 4;
      *(uint32_t*)NewGuestSP = Signal;
    }

    Frame->State.rip = reinterpret_cast<uint64_t>(GuestAction->sigaction_handler.handler);
  }

  if (CTX->Config.Is64BitMode) {
    Frame->State.gregs[X86State::REG_RDI] = Signal;

    // Set up the new SP for stack handling
    NewGuestSP -= 8;
    *(uint64_t*)NewGuestSP = CTX->X86CodeGen.SignalReturn;
    Frame->State.gregs[X86State::REG_RSP] = NewGuestSP;
  }
  else {
    NewGuestSP -= 4;
    *(uint32_t*)NewGuestSP = CTX->X86CodeGen.SignalReturn;
    LOGMAN_THROW_A(CTX->X86CodeGen.SignalReturn < 0x1'0000'0000ULL, "This needs to be below 4GB");
    Frame->State.gregs[X86State::REG_RSP] = NewGuestSP;
  }

  return true;
}

bool Dispatcher::HandleSIGILL(int Signal, void *info, void *ucontext) {

  if (ArchHelpers::Context::GetPc(ucontext) == SignalHandlerReturnAddress) {
    RestoreThreadState(ucontext);

    // Ref count our faults
    // We use this to track if it is safe to clear cache
    --SignalHandlerRefCounter;
    return true;
  }

  if (ArchHelpers::Context::GetPc(ucontext) == PauseReturnInstruction) {
    RestoreThreadState(ucontext);

    // Ref count our faults
    // We use this to track if it is safe to clear cache
    --SignalHandlerRefCounter;
    return true;
  }

  return false;
}

bool Dispatcher::HandleSignalPause(int Signal, void *info, void *ucontext) {
  FEXCore::Core::SignalEvent SignalReason = ThreadState->SignalReason.load();
  auto Frame = ThreadState->CurrentFrame;

  if (SignalReason == FEXCore::Core::SignalEvent::Pause) {
    // Store our thread state so we can come back to this
    StoreThreadState(Signal, ucontext);

    if (SRAEnabled && IsAddressInJITCode(ArchHelpers::Context::GetPc(ucontext), false)) {
      // We are in jit, SRA must be spilled
      ArchHelpers::Context::SetPc(ucontext, ThreadPauseHandlerAddressSpillSRA);
    } else {
      if (SRAEnabled) {
        // We are in non-jit, SRA is already spilled
        LOGMAN_THROW_A(!IsAddressInJITCode(ArchHelpers::Context::GetPc(ucontext), true), "Signals in dispatcher have unsynchronized context");
      }
      ArchHelpers::Context::SetPc(ucontext, ThreadPauseHandlerAddress);
    }

    // Set the new PC
    ArchHelpers::Context::SetPc(ucontext, ThreadPauseHandlerAddress);

    // Set our state register to point to our guest thread data
    ArchHelpers::Context::SetState(ucontext, reinterpret_cast<uint64_t>(Frame));

    // Ref count our faults
    // We use this to track if it is safe to clear cache
    ++SignalHandlerRefCounter;

    ThreadState->SignalReason.store(FEXCore::Core::SignalEvent::Nothing);
    return true;
  }

  if (SignalReason == FEXCore::Core::SignalEvent::Stop) {
    // Our thread is stopping
    // We don't care about anything at this point
    // Set the stack to our starting location when we entered the core and get out safely
    ArchHelpers::Context::SetSp(ucontext, Frame->ReturningStackLocation);

    // Our ref counting doesn't matter anymore
    SignalHandlerRefCounter = 0;

    // Set the new PC
    if (SRAEnabled && IsAddressInJITCode(ArchHelpers::Context::GetPc(ucontext), false)) {
      // We are in jit, SRA must be spilled
      ArchHelpers::Context::SetPc(ucontext, ThreadStopHandlerAddressSpillSRA);
    } else {
      if (SRAEnabled) {
        // We are in non-jit, SRA is already spilled
        LOGMAN_THROW_A(!IsAddressInJITCode(ArchHelpers::Context::GetPc(ucontext), true), "Signals in dispatcher have unsynchronized context");
      }
      ArchHelpers::Context::SetPc(ucontext, ThreadStopHandlerAddress);
    }

    ThreadState->SignalReason.store(FEXCore::Core::SignalEvent::Nothing);
    return true;
  }

  if (SignalReason == FEXCore::Core::SignalEvent::Return) {
    RestoreThreadState(ucontext);

    // Ref count our faults
    // We use this to track if it is safe to clear cache
    --SignalHandlerRefCounter;

    ThreadState->SignalReason.store(FEXCore::Core::SignalEvent::Nothing);
    return true;
  }

  return false;
}

uint64_t Dispatcher::GetCompileBlockPtr() {
  using ClassPtrType = void (FEXCore::Context::Context::*)(FEXCore::Core::CpuStateFrame *, uint64_t);
  union PtrCast {
    ClassPtrType ClassPtr;
    uintptr_t Data;
  };

  PtrCast CompileBlockPtr;
  CompileBlockPtr.ClassPtr = &FEXCore::Context::Context::CompileBlockJit;
  return CompileBlockPtr.Data;
}

void Dispatcher::RemoveCodeBuffer(uint8_t* start_to_remove) {
  for (auto iter = CodeBuffers.begin(); iter != CodeBuffers.end(); ++iter) {
    auto [start, end] = *iter;
    if (start == reinterpret_cast<uint64_t>(start_to_remove)) {
      CodeBuffers.erase(iter);
      return;
    }
  }
}

bool Dispatcher::IsAddressInJITCode(uint64_t Address, bool IncludeDispatcher) const {
  for (auto [start, end] : CodeBuffers) {
    if (Address >= start && Address < end) {
      return true;
    }
  }

  if (IncludeDispatcher) {
    return IsAddressInDispatcher(Address);
  }
  return false;
}

}
