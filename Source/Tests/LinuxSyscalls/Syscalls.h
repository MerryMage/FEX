/*
$info$
tags: LinuxSyscalls|common
desc: Glue logic, STRACE magic
$end_info$
*/

#pragma once

#include "Tests/LinuxSyscalls/FileManagement.h"
#include "Tests/LinuxSyscalls/SignalDelegator.h"
#include "git_version.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

#include <sys/epoll.h>

// #define DEBUG_STRACE

namespace FEXCore::Core {
struct CpuStateFrame;
}

namespace FEX::HLE {
class SyscallHandler;
  void RegisterEpoll();
  void RegisterFD(FEX::HLE::SyscallHandler *const Handler);
  void RegisterFS();
  void RegisterInfo();
  void RegisterIO();
  void RegisterKey();
  void RegisterMemory();
  void RegisterMsg();
  void RegisterNuma();
  void RegisterSched();
  void RegisterSemaphore();
  void RegisterSHM();
  void RegisterSignals();
  void RegisterSocket();
  void RegisterThread();
  void RegisterTime();
  void RegisterTimer();
  void RegisterNotImplemented();
  void RegisterStubs();

uint64_t UnimplementedSyscall(FEXCore::Core::CpuStateFrame *Frame, uint64_t SyscallNumber);
uint64_t UnimplementedSyscallSafe(FEXCore::Core::CpuStateFrame *Frame, uint64_t SyscallNumber);

uint64_t ExecveHandler(const char *pathname, std::vector<const char*> &argv, std::vector<const char*> &envp);

class SyscallHandler : public FEXCore::HLE::SyscallHandler {
public:
  SyscallHandler(FEXCore::Context::Context *ctx, FEX::HLE::SignalDelegator *_SignalDelegation);
  virtual ~SyscallHandler();

  // In the case that the syscall doesn't hit the optimized path then we still need to go here
  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame *Frame, FEXCore::HLE::SyscallArguments *Args) final override;

  void DefaultProgramBreak(uint64_t Base, uint64_t Size);

  using SyscallPtrArg0 = uint64_t(*)(FEXCore::Core::CpuStateFrame *Frame);
  using SyscallPtrArg1 = uint64_t(*)(FEXCore::Core::CpuStateFrame *Frame, uint64_t);
  using SyscallPtrArg2 = uint64_t(*)(FEXCore::Core::CpuStateFrame *Frame, uint64_t, uint64_t);
  using SyscallPtrArg3 = uint64_t(*)(FEXCore::Core::CpuStateFrame *Frame, uint64_t, uint64_t, uint64_t);
  using SyscallPtrArg4 = uint64_t(*)(FEXCore::Core::CpuStateFrame *Frame, uint64_t, uint64_t, uint64_t, uint64_t);
  using SyscallPtrArg5 = uint64_t(*)(FEXCore::Core::CpuStateFrame *Frame, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  using SyscallPtrArg6 = uint64_t(*)(FEXCore::Core::CpuStateFrame *Frame, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

  //virtual void *MemMap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = 0;

  struct SyscallFunctionDefinition {
    uint8_t NumArgs;
    union {
      void* Ptr;
      SyscallPtrArg0 Ptr0;
      SyscallPtrArg1 Ptr1;
      SyscallPtrArg2 Ptr2;
      SyscallPtrArg3 Ptr3;
      SyscallPtrArg4 Ptr4;
      SyscallPtrArg5 Ptr5;
      SyscallPtrArg6 Ptr6;
    };
#ifdef DEBUG_STRACE
    std::string StraceFmt;
#endif
  };

  SyscallFunctionDefinition const *GetDefinition(uint64_t Syscall) {
    return &Definitions.at(Syscall);
  }

  FEXCore::HLE::SyscallABI GetSyscallABI(uint64_t Syscall) override {
    auto &Def = Definitions.at(Syscall);
    return {Def.NumArgs, true};
  }

  uint64_t HandleBRK(FEXCore::Core::CpuStateFrame *Frame, void *Addr);

  FEX::HLE::FileManager FM;
  FEXCore::CodeLoader *GetCodeLoader() const { return LocalLoader; }
  void SetCodeLoader(FEXCore::CodeLoader *Loader) { LocalLoader = Loader; }
  FEX::HLE::SignalDelegator *GetSignalDelegator() { return SignalDelegation; }

  FEX_CONFIG_OPT(IsInterpreter, IS_INTERPRETER);
  FEX_CONFIG_OPT(IsInterpreterInstalled, INTERPRETER_INSTALLED);
  FEX_CONFIG_OPT(Filename, APP_FILENAME);
  FEX_CONFIG_OPT(RootFSPath, ROOTFS);
  FEX_CONFIG_OPT(ThreadsConfig, THREADS);
  FEX_CONFIG_OPT(Is64BitMode, IS64BIT_MODE);

  uint32_t GetHostKernelVersion() const { return HostKernelVersion; }

  static uint32_t CalculateHostKernelVersion();
  static uint32_t KernelVersion(uint32_t Major, uint32_t Minor = 0, uint32_t Patch = 0) {
    return (Major << 24) | (Minor << 16) | Patch;
  }

protected:
  std::vector<SyscallFunctionDefinition> Definitions{};
  std::mutex MMapMutex;

  // BRK management
  uint64_t DataSpace {};
  uint64_t DataSpaceSize {};
  uint64_t DataSpaceMaxSize {};
  uint64_t DataSpaceStartingSize{};

  // (Major << 24) | (Minor << 16) | Patch
  uint32_t HostKernelVersion{};

private:

  FEX::HLE::SignalDelegator *SignalDelegation;

  std::mutex FutexMutex;
  std::mutex SyscallMutex;
  FEXCore::CodeLoader *LocalLoader{};

  #ifdef DEBUG_STRACE
    void Strace(FEXCore::HLE::SyscallArguments *Args, uint64_t Ret);
  #endif

};

FEX::HLE::SyscallHandler *CreateHandler(FEXCore::Context::OperatingMode Mode,
  FEXCore::Context::Context *ctx,
  FEX::HLE::SignalDelegator *_SignalDelegation,
  FEXCore::CodeLoader *Loader
  );
uint64_t HandleSyscall(SyscallHandler *Handler, FEXCore::Core::CpuStateFrame *Frame, FEXCore::HLE::SyscallArguments *Args);

#define SYSCALL_ERRNO() do { if (Result == -1) return -errno; return Result; } while(0)
#define SYSCALL_ERRNO_NULL() do { if (Result == 0) return -errno; return Result; } while(0)

extern FEX::HLE::SyscallHandler *_SyscallHandler;

#ifdef DEBUG_STRACE
//////
/// Templates to map parameters to format string for syscalls
//////

template<typename T>
struct ArgToFmtString {
  // fail on unknown types
};

#define ARG_TO_STR(tpy, str) template<> struct FEX::HLE::ArgToFmtString<tpy> { inline static const std::string Format = str; };

// Base types
ARG_TO_STR(int, "%d")
ARG_TO_STR(unsigned int, "%u")
ARG_TO_STR(long, "%ld")
ARG_TO_STR(unsigned long, "%lu")

//string types
ARG_TO_STR(char*, "%s")
ARG_TO_STR(const char*, "%s")

// Pointers
template<typename T>
struct ArgToFmtString<T*> {
  inline static const std::string Format = "%p";
};

// Use ArgToFmtString and variadic template to create a format string from an args list
template<typename ...Args>
std::string CollectArgsFmtString() {
  std::string array[] = { ArgToFmtString<Args>::Format... };

  std::string rv{};
  bool first = true;

  for (auto &str: array) {
    if (!first) rv += ", ";
    first = false;
    rv += str;
  }

  return rv;
}
#else
#define ARG_TO_STR(tpy, str)
#endif

/////
// REGISTER_SYSCALL_FORWARD_ERRNO implementation
// Given a syscall wrapper, it generate a syscall implementation using the wrapper's signature, forward the arguments
// and register to syscalls via RegisterSyscall
/////

// Helper that allows us to create a variadic template lambda from a given signature
// by creating a function that expects a fuction pointer with the given signature as a parameter
template <typename T>
struct FunctionToLambda;

template<typename R, typename... Args>
struct FunctionToLambda<R(*)(Args...)> {
	using RType = R;

	static R(*ReturnFunctionPointer(R(*fn)(FEXCore::Core::CpuStateFrame *Frame, Args...)))(FEXCore::Core::CpuStateFrame *Frame, Args...) {
		return fn;
	}
};

// copy to match noexcept functions
template<typename R, typename... Args>
struct FunctionToLambda<R(*)(Args...) noexcept> {
	using RType = R;

	static R(*ReturnFunctionPointer(R(*fn)(FEXCore::Core::CpuStateFrame *Frame, Args...)))(FEXCore::Core::CpuStateFrame *Frame, Args...) {
		return fn;
	}
};

struct __attribute__((packed)) epoll_event_x86 {
  uint32_t events;
  epoll_data_t data;

  epoll_event_x86() = delete;

  operator struct epoll_event() const {
    epoll_event event{};
    event.events = events;
    event.data = data;
    return event;
  }

  epoll_event_x86(struct epoll_event event) {
    events = event.events;
    data = event.data;
  }
};
static_assert(std::is_trivial<epoll_event_x86>::value, "Needs to be trivial");
static_assert(sizeof(epoll_event_x86) == 12, "Incorrect size");
}

// Creates a variadic template lambda from a global function (via FunctionToLambda), then forwards the arguments to the specified function
// also handles errno
#define SYSCALL_FORWARD_ERRNO(function) \
  FEX::HLE::FunctionToLambda<decltype(&::function)>::ReturnFunctionPointer([](FEXCore::Core::CpuStateFrame *Frame, auto... Args) { \
    FEX::HLE::FunctionToLambda<decltype(&::function)>::RType Result = ::function(Args...); \
    do { if (Result == -1) return (FEX::HLE::FunctionToLambda<decltype(&::function)>::RType)-errno; return Result; } while(0); \
  })

// Helpers to register a syscall implementation
// Creates a syscall forward from a glibc wrapper, and registers it
#define REGISTER_SYSCALL_FORWARD_ERRNO(function) do { \
  FEX::HLE::x64::RegisterSyscall(FEX::HLE::x64::SYSCALL_x64_##function, #function, SYSCALL_FORWARD_ERRNO(function)); \
  FEX::HLE::x32::RegisterSyscall(FEX::HLE::x32::SYSCALL_x86_##function, #function, SYSCALL_FORWARD_ERRNO(function)); \
  } while(0)

// Registers syscall for both 32bit and 64bit
#define REGISTER_SYSCALL_IMPL(name, lambda) \
  struct impl_##name { \
    impl_##name() \
    { \
      FEX::HLE::x64::RegisterSyscall(FEX::HLE::x64::SYSCALL_x64_##name, #name, lambda); \
      FEX::HLE::x32::RegisterSyscall(FEX::HLE::x32::SYSCALL_x86_##name, #name, lambda); \
    } } impl_##name

