// SPDX-License-Identifier: MIT
/*
$info$
glossary: Splatter ~ a code generator backend that concaternates configurable macros instead of doing isel
glossary: IR ~ Intermediate Representation, our high-level opcode representation, loosely modeling arm64
glossary: SSA ~ Single Static Assignment, a form of representing IR in memory
glossary: Basic Block ~ A block of instructions with no control flow, terminated by control flow
glossary: Fragment ~ A Collection of basic blocks, possibly an entire guest function or a subset of it
tags: backend|arm64
desc: Main glue logic of the arm64 splatter backend
$end_info$
*/

#include "Common/SoftFloat.h"
#include "FEXCore/Utils/Telemetry.h"
#include "Interface/Context/Context.h"
#include "Interface/Core/LookupCache.h"

#include "Interface/Core/Dispatcher/Dispatcher.h"
#include "Interface/Core/JIT/JITClass.h"

#include "Interface/IR/Passes/RegisterAllocationPass.h"

#include "Utils/MemberFunctionToPointer.h"
#include "Utils/variable_length_integer.h"

#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/EnumUtils.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include "Interface/Core/Interpreter/InterpreterOps.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits>

static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 16;
// We don't want to move above 128MB atm because that means we will have to encode longer jumps
static constexpr size_t MAX_CODE_SIZE = 1024 * 1024 * 128;

namespace {
static uint64_t LUDIV(uint64_t SrcHigh, uint64_t SrcLow, uint64_t Divisor) {
  __uint128_t Source = (static_cast<__uint128_t>(SrcHigh) << 64) | SrcLow;
  __uint128_t Res = Source / Divisor;
  return Res;
}

static int64_t LDIV(uint64_t SrcHigh, uint64_t SrcLow, int64_t Divisor) {
  __int128_t Source = (static_cast<__uint128_t>(SrcHigh) << 64) | SrcLow;
  __int128_t Res = Source / Divisor;
  return Res;
}

static uint64_t LUREM(uint64_t SrcHigh, uint64_t SrcLow, uint64_t Divisor) {
  __uint128_t Source = (static_cast<__uint128_t>(SrcHigh) << 64) | SrcLow;
  __uint128_t Res = Source % Divisor;
  return Res;
}

static int64_t LREM(uint64_t SrcHigh, uint64_t SrcLow, int64_t Divisor) {
  __int128_t Source = (static_cast<__uint128_t>(SrcHigh) << 64) | SrcLow;
  __int128_t Res = Source % Divisor;
  return Res;
}

static void PrintValue(uint64_t Value) {
  LogMan::Msg::DFmt("Value: 0x{:x}", Value);
}

static void PrintVectorValue(uint64_t Value, uint64_t ValueUpper) {
  LogMan::Msg::DFmt("Value: 0x{:016x}'{:016x}", ValueUpper, Value);
}
} // namespace

namespace FEXCore::CPU {

void Arm64JITCore::Op_Unhandled(const IR::IROp_Header* IROp, IR::NodeID Node) {
  FallbackInfo Info;
  if (!InterpreterOps::GetFallbackHandler(CTX->HostFeatures.SupportsPreserveAllABI, IROp, &Info)) {
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    LOGMAN_MSG_A_FMT("Unhandled IR Op: {}", FEXCore::IR::GetName(IROp->Op));
#endif
  } else {
    auto FillF80Result = [&]() {
      if (!TMP_ABIARGS) {
        mov(VTMP1.Q(), ARMEmitter::VReg::v0.Q());
      }

      FillForABICall(Info.SupportsPreserveAllABI, true);

      const auto Dst = GetVReg(Node);
      mov(Dst.Q(), VTMP1.Q());
    };

    auto FillF64Result = [&]() {
      if (!TMP_ABIARGS) {
        mov(VTMP1.D(), ARMEmitter::DReg::d0);
      }
      FillForABICall(Info.SupportsPreserveAllABI, true);

      const auto Dst = GetVReg(Node);
      mov(Dst.D(), VTMP1.D());
    };

    auto FillI32Result = [&]() {
      if (!TMP_ABIARGS) {
        mov(TMP1.W(), ARMEmitter::WReg::w0);
      }
      FillForABICall(Info.SupportsPreserveAllABI, true);

      const auto Dst = GetReg(Node);
      mov(Dst.W(), TMP1.W());
    };

    switch (Info.ABI) {
    case FABI_F80_I16_F32_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());
      fmov(ARMEmitter::SReg::s0, Src1.S());
      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::XReg::x1, STATE);
      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<FEXCore::VectorRegType, uint16_t, float, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      FillF80Result();
    } break;

    case FABI_F80_I16_F64_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());
      mov(ARMEmitter::DReg::d0, Src1.D());
      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::XReg::x1, STATE);
      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<FEXCore::VectorRegType, uint16_t, double, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      FillF80Result();
    } break;

    case FABI_F80_I16_I16_PTR:
    case FABI_F80_I16_I32_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetReg(IROp->Args[0].ID());
      if (Info.ABI == FABI_F80_I16_I16_PTR) {
        sxth(ARMEmitter::Size::i32Bit, ARMEmitter::Reg::r1, Src1);
      } else {
        mov(ARMEmitter::Size::i32Bit, ARMEmitter::Reg::r1, Src1);
      }
      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::XReg::x2, STATE);
      ldr(ARMEmitter::XReg::x3, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<FEXCore::VectorRegType, uint16_t, uint32_t, uint64_t>(ARMEmitter::Reg::r3);
      } else {
        blr(ARMEmitter::Reg::r3);
      }

      FillF80Result();
    } break;

    case FABI_F32_I16_F80_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::VReg::v0.Q(), Src1.Q());

      mov(ARMEmitter::XReg::x1, STATE);
      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<float, uint16_t, FEXCore::VectorRegType, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      if (!TMP_ABIARGS) {
        fmov(VTMP1.S(), ARMEmitter::SReg::s0);
      }
      FillForABICall(Info.SupportsPreserveAllABI, true);

      const auto Dst = GetVReg(Node);
      fmov(Dst.S(), VTMP1.S());
    } break;

    case FABI_F64_I16_F80_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::VReg::v0.Q(), Src1.Q());
      mov(ARMEmitter::XReg::x1, STATE);

      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<double, uint16_t, FEXCore::VectorRegType, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      FillF64Result();
    } break;

    case FABI_F64_I16_F64_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());

      mov(ARMEmitter::DReg::d0, Src1.D());
      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::XReg::x1, STATE);

      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<double, uint16_t, double, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      FillF64Result();
    } break;

    case FABI_F64_I16_F64_F64_PTR: {
      const auto Src1 = GetVReg(IROp->Args[0].ID());
      const auto Src2 = GetVReg(IROp->Args[1].ID());

      mov(VTMP1.D(), Src1.D());
      mov(VTMP2.D(), Src2.D());

      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      if (!TMP_ABIARGS) {
        mov(ARMEmitter::DReg::d0, VTMP1.D());
        mov(ARMEmitter::DReg::d1, VTMP2.D());
      }

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::XReg::x1, STATE);
      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<double, uint16_t, double, double, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      FillF64Result();
    } break;

    case FABI_I16_I16_F80_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::VReg::v0.Q(), Src1.Q());
      mov(ARMEmitter::XReg::x1, STATE);

      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<uint32_t, uint16_t, FEXCore::VectorRegType, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      if (!TMP_ABIARGS) {
        mov(TMP1, ARMEmitter::XReg::x0);
      }
      FillForABICall(Info.SupportsPreserveAllABI, true);

      const auto Dst = GetReg(Node);
      sxth(ARMEmitter::Size::i64Bit, Dst, TMP1);
    } break;
    case FABI_I32_I16_F80_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::VReg::v0.Q(), Src1.Q());
      mov(ARMEmitter::XReg::x1, STATE);

      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<uint32_t, uint16_t, FEXCore::VectorRegType, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      FillI32Result();
    } break;
    case FABI_I64_I16_F80_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::VReg::v0.Q(), Src1.Q());
      mov(ARMEmitter::XReg::x1, STATE);

      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<uint64_t, uint16_t, FEXCore::VectorRegType, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      if (!TMP_ABIARGS) {
        mov(TMP1, ARMEmitter::XReg::x0);
      }
      FillForABICall(Info.SupportsPreserveAllABI, true);

      const auto Dst = GetReg(Node);
      mov(ARMEmitter::Size::i64Bit, Dst, TMP1);
    } break;
    case FABI_I64_I16_F80_F80_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());
      const auto Src2 = GetVReg(IROp->Args[1].ID());

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      if (!TMP_ABIARGS) {
        mov(VTMP1.Q(), Src1.Q());
        mov(ARMEmitter::VReg::v1.Q(), Src2.Q());
        mov(ARMEmitter::VReg::v0.Q(), VTMP1.Q());
      } else {
        mov(ARMEmitter::VReg::v0.Q(), Src1.Q());
        mov(ARMEmitter::VReg::v1.Q(), Src2.Q());
      }
      mov(ARMEmitter::XReg::x1, STATE);

      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<uint64_t, uint16_t, FEXCore::VectorRegType, FEXCore::VectorRegType, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      if (!TMP_ABIARGS) {
        mov(TMP1, ARMEmitter::XReg::x0);
      }
      FillForABICall(Info.SupportsPreserveAllABI, true);

      const auto Dst = GetReg(Node);
      mov(ARMEmitter::Size::i64Bit, Dst, TMP1);
    } break;
    case FABI_F80_I16_F80_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      mov(ARMEmitter::XReg::x1, STATE);
      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      mov(ARMEmitter::VReg::v0.Q(), Src1.Q());

      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<FEXCore::VectorRegType, uint16_t, FEXCore::VectorRegType, uint64_t>(ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      FillF80Result();
    } break;
    case FABI_F80_I16_F80_F80_PTR: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Src1 = GetVReg(IROp->Args[0].ID());
      const auto Src2 = GetVReg(IROp->Args[1].ID());

      ldrh(ARMEmitter::WReg::w0, STATE, offsetof(FEXCore::Core::CPUState, FCW));
      if (!TMP_ABIARGS) {
        mov(VTMP1.Q(), Src1.Q());
        mov(ARMEmitter::VReg::v1.Q(), Src2.Q());
        mov(ARMEmitter::VReg::v0.Q(), VTMP1.Q());
      } else {
        mov(ARMEmitter::VReg::v0.Q(), Src1.Q());
        mov(ARMEmitter::VReg::v1.Q(), Src2.Q());
      }

      mov(ARMEmitter::XReg::x1, STATE);
      ldr(ARMEmitter::XReg::x2, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<FEXCore::VectorRegType, uint16_t, FEXCore::VectorRegType, FEXCore::VectorRegType, uint64_t>(
          ARMEmitter::Reg::r2);
      } else {
        blr(ARMEmitter::Reg::r2);
      }

      FillF80Result();
    } break;
    case FABI_I32_I64_I64_I128_I128_I16: {
      const auto Op = IROp->C<IR::IROp_VPCMPESTRX>();
      const auto SrcRAX = GetReg(Op->RAX.ID());
      const auto SrcRDX = GetReg(Op->RDX.ID());

      mov(TMP1, SrcRAX.X());
      mov(TMP2, SrcRDX.X());

      SpillForABICall(Info.SupportsPreserveAllABI, TMP3, true);

      const auto Control = Op->Control;

      const auto Src1 = GetVReg(Op->LHS.ID());
      const auto Src2 = GetVReg(Op->RHS.ID());

      if (!TMP_ABIARGS) {
        mov(ARMEmitter::XReg::x0, TMP1);
        mov(ARMEmitter::XReg::x1, TMP2);
      }

      umov<ARMEmitter::SubRegSize::i64Bit>(ARMEmitter::Reg::r2, Src1, 0);
      umov<ARMEmitter::SubRegSize::i64Bit>(ARMEmitter::Reg::r3, Src1, 1);

      umov<ARMEmitter::SubRegSize::i64Bit>(ARMEmitter::Reg::r4, Src2, 0);
      umov<ARMEmitter::SubRegSize::i64Bit>(ARMEmitter::Reg::r5, Src2, 1);

      movz(ARMEmitter::Size::i32Bit, ARMEmitter::Reg::r6, Control);

      ldr(ARMEmitter::XReg::x7, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint16_t>(ARMEmitter::Reg::r7);
      } else {
        blr(ARMEmitter::Reg::r7);
      }

      FillI32Result();
    } break;
    case FABI_I32_V128_V128_I16: {
      SpillForABICall(Info.SupportsPreserveAllABI, TMP1, true);

      const auto Op = IROp->C<IR::IROp_VPCMPISTRX>();

      const auto Src1 = GetVReg(Op->LHS.ID());
      const auto Src2 = GetVReg(Op->RHS.ID());
      const auto Control = Op->Control;

      if (!TMP_ABIARGS) {
        mov(VTMP1.Q(), Src1.Q());
        mov(ARMEmitter::VReg::v1.Q(), Src2.Q());
        mov(ARMEmitter::VReg::v0.Q(), VTMP1.Q());
      } else {
        mov(ARMEmitter::VReg::v0.Q(), Src1.Q());
        mov(ARMEmitter::VReg::v1.Q(), Src2.Q());
      }

      movz(ARMEmitter::Size::i32Bit, ARMEmitter::Reg::r0, Control);

      ldr(ARMEmitter::XReg::x1, STATE_PTR(CpuStateFrame, Pointers.Common.FallbackHandlerPointers[Info.HandlerIndex]));
      if (!CTX->Config.DisableVixlIndirectCalls) [[unlikely]] {
        GenerateIndirectRuntimeCall<uint32_t, FEXCore::VectorRegType, FEXCore::VectorRegType, uint16_t>(ARMEmitter::Reg::r1);
      } else {
        blr(ARMEmitter::Reg::r1);
      }

      FillI32Result();
    } break;
    case FABI_UNKNOWN:
    default:
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
      LOGMAN_MSG_A_FMT("Unhandled IR Fallback ABI: {} {}", FEXCore::IR::GetName(IROp->Op), ToUnderlying(Info.ABI));
#endif
      break;
    }
  }
}

static void DirectBlockDelinker(FEXCore::Core::CpuStateFrame* Frame, FEXCore::Context::ExitFunctionLinkData* Record) {
  auto LinkerAddress = Frame->Pointers.Common.ExitFunctionLinker;
  uintptr_t branch = (uintptr_t)(Record)-8;
  ARMEmitter::Emitter emit((uint8_t*)(branch), 8);
  ARMEmitter::ForwardLabel l_BranchHost;
  emit.ldr(TMP1, &l_BranchHost);
  emit.blr(TMP1);
  emit.Bind(&l_BranchHost);
  emit.dc64(LinkerAddress);
  ARMEmitter::Emitter::ClearICache((void*)branch, 8);
}

static void IndirectBlockDelinker(FEXCore::Core::CpuStateFrame* Frame, FEXCore::Context::ExitFunctionLinkData* Record) {
  auto LinkerAddress = Frame->Pointers.Common.ExitFunctionLinker;
  Record->HostBranch = LinkerAddress;
}

static uint64_t Arm64JITCore_ExitFunctionLink(FEXCore::Core::CpuStateFrame* Frame, FEXCore::Context::ExitFunctionLinkData* Record) {
  auto Thread = Frame->Thread;
  bool TFSet = Thread->CurrentFrame->State.flags[X86State::RFLAG_TF_RAW_LOC];
  uintptr_t HostCode {};
  auto GuestRip = Record->GuestRIP;

  if (!TFSet) {
    HostCode = Thread->LookupCache->FindBlock(GuestRip);
  }

  if (TFSet || !HostCode) {
    // If TF is set, the cache must be skipped as different code needs to be generated.
    Frame->State.rip = GuestRip;
    return Frame->Pointers.Common.DispatcherLoopTop;
  }

  uintptr_t branch = (uintptr_t)(Record)-8;

  auto offset = HostCode / 4 - branch / 4;
  if (ARMEmitter::Emitter::IsInt26(offset)) {
    // optimal case - can branch directly
    // patch the code
    ARMEmitter::Emitter emit((uint8_t*)(branch), 4);
    emit.b(offset);
    ARMEmitter::Emitter::ClearICache((void*)branch, 4);

    // Add de-linking handler
    Thread->LookupCache->AddBlockLink(GuestRip, Record, DirectBlockDelinker);
  } else {
    // fallback case - do a soft-er link by patching the pointer
    Record->HostBranch = HostCode;

    // Add de-linking handler
    Thread->LookupCache->AddBlockLink(GuestRip, Record, IndirectBlockDelinker);
  }

  return HostCode;
}

void Arm64JITCore::Op_NoOp(const IR::IROp_Header* IROp, IR::NodeID Node) {}

Arm64JITCore::Arm64JITCore(FEXCore::Context::ContextImpl* ctx, FEXCore::Core::InternalThreadState* Thread)
  : CPUBackend(Thread, INITIAL_CODE_SIZE, MAX_CODE_SIZE)
  , Arm64Emitter(ctx)
  , HostSupportsSVE128 {ctx->HostFeatures.SupportsSVE128}
  , HostSupportsSVE256 {ctx->HostFeatures.SupportsSVE256}
  , HostSupportsAVX256 {ctx->HostFeatures.SupportsAVX && ctx->HostFeatures.SupportsSVE256}
  , HostSupportsRPRES {ctx->HostFeatures.SupportsRPRES}
  , HostSupportsAFP {ctx->HostFeatures.SupportsAFP}
  , CTX {ctx} {

  RAPass = Thread->PassManager->GetPass<IR::RegisterAllocationPass>("RA");

  RAPass->AddRegisters(FEXCore::IR::GPRClass, GeneralRegisters.size());
  RAPass->AddRegisters(FEXCore::IR::GPRFixedClass, StaticRegisters.size());
  RAPass->AddRegisters(FEXCore::IR::FPRClass, GeneralFPRegisters.size());
  RAPass->AddRegisters(FEXCore::IR::FPRFixedClass, StaticFPRegisters.size());
  RAPass->PairRegs = PairRegisters;

  {
    // Set up pointers that the JIT needs to load

    // Common
    auto& Common = ThreadState->CurrentFrame->Pointers.Common;

    Common.PrintValue = reinterpret_cast<uint64_t>(PrintValue);
    Common.PrintVectorValue = reinterpret_cast<uint64_t>(PrintVectorValue);
    Common.ThreadRemoveCodeEntryFromJIT = reinterpret_cast<uintptr_t>(&Context::ContextImpl::ThreadRemoveCodeEntryFromJit);
    Common.CPUIDObj = reinterpret_cast<uint64_t>(&CTX->CPUID);

    {
      FEXCore::Utils::MemberFunctionToPointerCast PMF(&FEXCore::CPUIDEmu::RunFunction);
      Common.CPUIDFunction = PMF.GetConvertedPointer();
    }

    {
      FEXCore::Utils::MemberFunctionToPointerCast PMF(&FEXCore::CPUIDEmu::RunXCRFunction);
      Common.XCRFunction = PMF.GetConvertedPointer();
    }

    {
      FEXCore::Utils::MemberFunctionToPointerCast PMF(&FEXCore::HLE::SyscallHandler::HandleSyscall);
      Common.SyscallHandlerObj = reinterpret_cast<uint64_t>(CTX->SyscallHandler);
      Common.SyscallHandlerFunc = PMF.GetVTableEntry(CTX->SyscallHandler);
    }
    Common.ExitFunctionLink = reinterpret_cast<uintptr_t>(&Context::ContextImpl::ThreadExitFunctionLink<Arm64JITCore_ExitFunctionLink>);

    // Fill in the fallback handlers
    InterpreterOps::FillFallbackIndexPointers(Common.FallbackHandlerPointers);

    // Platform Specific
    auto& AArch64 = ThreadState->CurrentFrame->Pointers.AArch64;

    AArch64.LUDIV = reinterpret_cast<uint64_t>(LUDIV);
    AArch64.LDIV = reinterpret_cast<uint64_t>(LDIV);
    AArch64.LUREM = reinterpret_cast<uint64_t>(LUREM);
    AArch64.LREM = reinterpret_cast<uint64_t>(LREM);
  }

  // Must be done after Dispatcher init
  ClearCache();

  // Setup dynamic dispatch.
  if (ParanoidTSO()) {
    RT_LoadMemTSO = &Arm64JITCore::Op_ParanoidLoadMemTSO;
    RT_StoreMemTSO = &Arm64JITCore::Op_ParanoidStoreMemTSO;
  } else {
    RT_LoadMemTSO = &Arm64JITCore::Op_LoadMemTSO;
    RT_StoreMemTSO = &Arm64JITCore::Op_StoreMemTSO;
  }
}

void Arm64JITCore::EmitDetectionString() {
  const char JITString[] = "FEXJIT::Arm64JITCore::";
  EmitString(JITString);
  Align();
}

void Arm64JITCore::ClearCache() {
  // Get the backing code buffer

  auto CodeBuffer = GetEmptyCodeBuffer();
  SetBuffer(CodeBuffer->Ptr, CodeBuffer->Size);
  EmitDetectionString();
}

Arm64JITCore::~Arm64JITCore() {}

bool Arm64JITCore::IsInlineConstant(const IR::OrderedNodeWrapper& WNode, uint64_t* Value) const {
  auto OpHeader = IR->GetOp<IR::IROp_Header>(WNode);

  if (OpHeader->Op == IR::IROps::OP_INLINECONSTANT) {
    auto Op = OpHeader->C<IR::IROp_InlineConstant>();
    if (Value) {
      *Value = Op->Constant;
    }
    return true;
  } else {
    return false;
  }
}

bool Arm64JITCore::IsInlineEntrypointOffset(const IR::OrderedNodeWrapper& WNode, uint64_t* Value) const {
  auto OpHeader = IR->GetOp<IR::IROp_Header>(WNode);

  if (OpHeader->Op == IR::IROps::OP_INLINEENTRYPOINTOFFSET) {
    auto Op = OpHeader->C<IR::IROp_InlineEntrypointOffset>();
    if (Value) {
      uint64_t Mask = ~0ULL;
      const auto Size = OpHeader->Size;
      if (Size == IR::OpSize::i32Bit) {
        Mask = 0xFFFF'FFFFULL;
      }
      *Value = (Entry + Op->Offset) & Mask;
    }
    return true;
  } else {
    return false;
  }
}

FEXCore::IR::RegisterClassType Arm64JITCore::GetRegClass(IR::NodeID Node) const {
  return FEXCore::IR::RegisterClassType {GetPhys(Node).Class};
}

bool Arm64JITCore::IsFPR(IR::NodeID Node) const {
  auto Class = GetRegClass(Node);

  return Class == IR::FPRClass || Class == IR::FPRFixedClass;
}

bool Arm64JITCore::IsGPR(IR::NodeID Node) const {
  auto Class = GetRegClass(Node);

  return Class == IR::GPRClass || Class == IR::GPRFixedClass;
}

void Arm64JITCore::EmitInterruptChecks(bool CheckTF) {
  if (CheckTF) {
    ARMEmitter::ForwardLabel l_TFUnset;
    ARMEmitter::ForwardLabel l_TFBlocked;

    // Note that this needs to be before the below suspend checks, as X86 checks this flag immediately after executing an instruction.
    ldrb(TMP1, STATE_PTR(CpuStateFrame, State.flags[X86State::RFLAG_TF_RAW_LOC]));

    cbz(ARMEmitter::Size::i32Bit, TMP1, &l_TFUnset);

    // X86 semantically checks TF after executing each instruction, so e.g. setting a context with TF set will execute a single instruction
    // and then raise an exception. However on the FEX side this is simpler to implement by checking at the start of each instruction, handle this by having bit 1 being unset in the flag state indicate that TF is blocked for a single instruction.
    tbz(TMP1, 1, &l_TFBlocked);

    // Block TF for a single instruction when the frontend jumps to a new context by unsetting bit 1.
    ldrb(TMP1, STATE_PTR(CpuStateFrame, State.flags[X86State::RFLAG_TF_RAW_LOC]));
    and_(ARMEmitter::Size::i32Bit, TMP1, TMP1, ~(1 << 1));
    strb(TMP1, STATE_PTR(CpuStateFrame, State.flags[X86State::RFLAG_TF_RAW_LOC]));

    Core::CpuStateFrame::SynchronousFaultDataStruct State = {
      .FaultToTopAndGeneratedException = 1,
      .Signal = Core::FAULT_SIGTRAP,
      .TrapNo = X86State::X86_TRAPNO_DB,
      .si_code = 2,
      .err_code = 0,
    };

    uint64_t Constant {};
    memcpy(&Constant, &State, sizeof(State));

    LoadConstant(ARMEmitter::Size::i64Bit, TMP1, Constant);
    str(TMP1, STATE, offsetof(FEXCore::Core::CpuStateFrame, SynchronousFaultData));
    ldr(TMP1, STATE, offsetof(FEXCore::Core::CpuStateFrame, Pointers.Common.GuestSignal_SIGTRAP));
    br(TMP1);

    Bind(&l_TFBlocked);
    // If TF was blocked for this instruction, unblock it for the next.
    LoadConstant(ARMEmitter::Size::i32Bit, TMP1, 0b11);
    strb(TMP1, STATE_PTR(CpuStateFrame, State.flags[X86State::RFLAG_TF_RAW_LOC]));
    Bind(&l_TFUnset);
  }

  if (CTX->Config.NeedsPendingInterruptFaultCheck) {
    // Trigger a fault if there are any pending interrupts
    // Used only for suspend on WIN32 at the moment
    strb(ARMEmitter::XReg::zr, STATE,
         offsetof(FEXCore::Core::InternalThreadState, InterruptFaultPage) - offsetof(FEXCore::Core::InternalThreadState, BaseFrameState));
  }

#ifdef _M_ARM_64EC
  static constexpr uint16_t SuspendMagic {0xCAFE};

  ldr(TMP2.W(), STATE_PTR(CpuStateFrame, SuspendDoorbell));
  ARMEmitter::ForwardLabel l_NoSuspend;
  cbz(ARMEmitter::Size::i32Bit, TMP2, &l_NoSuspend);
  brk(SuspendMagic);
  Bind(&l_NoSuspend);
#endif
}

CPUBackend::CompiledCode Arm64JITCore::CompileCode(uint64_t Entry, uint64_t Size, bool SingleInst, const FEXCore::IR::IRListView* IR,
                                                   FEXCore::Core::DebugData* DebugData, const FEXCore::IR::RegisterAllocationData* RAData,
                                                   bool CheckTF) {
  FEXCORE_PROFILE_SCOPED("Arm64::CompileCode");

  JumpTargets.clear();
  uint32_t SSACount = IR->GetSSACount();

  this->Entry = Entry;
  this->RAData = RAData;
  this->DebugData = DebugData;
  this->IR = IR;

  // Fairly excessive buffer range to make sure we don't overflow
  uint32_t BufferRange = SSACount * 16;
  if ((GetCursorOffset() + BufferRange) > CurrentCodeBuffer->Size) {
    CTX->ClearCodeCache(ThreadState);
  }

  CodeData.BlockBegin = GetCursorAddress<uint8_t*>();

  // Put the code header at the start of the data block.
  ARMEmitter::BackwardLabel JITCodeHeaderLabel {};
  Bind(&JITCodeHeaderLabel);
  JITCodeHeader* CodeHeader = GetCursorAddress<JITCodeHeader*>();
  CursorIncrement(sizeof(JITCodeHeader));

#ifdef VIXL_DISASSEMBLER
  const auto DisasmBegin = GetCursorAddress<const vixl::aarch64::Instruction*>();
#endif

  // AAPCS64
  // r30      = LR
  // r29      = FP
  // r19..r28 = Callee saved
  // r18      = Platform Register (Matters if we target Windows or iOS)
  // r16..r17 = Inter-procedure scratch
  //  r9..r15 = Temp
  //  r8      = Indirect Result
  //  r0...r7 = Parameter/Results
  //
  //  FPRS:
  //  v8..v15 = (lower 64bits) Callee saved

  // Our allocation:
  // X0 = ThreadState
  // X1 = MemBase
  //
  // X1-X3 = Temp
  // X4-r18 = RA

  CodeData.BlockEntry = GetCursorAddress<uint8_t*>();

  // Get the address of the JITCodeHeader and store in to the core state.
  // Two instruction cost, each 1 cycle.
  adr(TMP1, &JITCodeHeaderLabel);
  str(TMP1, STATE, offsetof(FEXCore::Core::CPUState, InlineJITBlockHeader));

  EmitInterruptChecks(CheckTF);

  SpillSlots = RAData->SpillSlots();

  if (SpillSlots) {
    const auto TotalSpillSlotsSize = SpillSlots * MaxSpillSlotSize;

    if (ARMEmitter::IsImmAddSub(TotalSpillSlotsSize)) {
      sub(ARMEmitter::Size::i64Bit, ARMEmitter::Reg::rsp, ARMEmitter::Reg::rsp, TotalSpillSlotsSize);
    } else {
      LoadConstant(ARMEmitter::Size::i64Bit, TMP1, TotalSpillSlotsSize);
      sub(ARMEmitter::Size::i64Bit, ARMEmitter::XReg::rsp, ARMEmitter::XReg::rsp, TMP1, ARMEmitter::ExtendedType::LSL_64, 0);
    }
  }

  PendingTargetLabel = nullptr;

  for (auto [BlockNode, BlockHeader] : IR->GetBlocks()) {
    using namespace FEXCore::IR;
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    auto BlockIROp = BlockHeader->CW<FEXCore::IR::IROp_CodeBlock>();
    LOGMAN_THROW_A_FMT(BlockIROp->Header.Op == IR::OP_CODEBLOCK, "IR type failed to be a code block");
#endif

    auto BlockStartHostCode = GetCursorAddress<uint8_t*>();
    {
      const auto Node = IR->GetID(BlockNode);
      const auto IsTarget = JumpTargets.try_emplace(Node).first;

      // if there's a pending branch, and it is not fall-through
      if (PendingTargetLabel && PendingTargetLabel != &IsTarget->second) {
        b(PendingTargetLabel);
      }
      PendingTargetLabel = nullptr;

      Bind(&IsTarget->second);
    }

    for (auto [CodeNode, IROp] : IR->GetCode(BlockNode)) {
      const auto ID = IR->GetID(CodeNode);
      switch (IROp->Op) {
#define REGISTER_OP_RT(op, x) \
  case FEXCore::IR::IROps::OP_##op: std::invoke(RT_##x, this, IROp, ID); break
#define REGISTER_OP(op, x) \
  case FEXCore::IR::IROps::OP_##op: Op_##x(IROp, ID); break

#define IROP_DISPATCH_DISPATCH
#include <FEXCore/IR/IRDefines_Dispatch.inc>
#undef REGISTER_OP

      default: Op_Unhandled(IROp, ID); break;
      }
    }

    if (DebugData) {
      DebugData->Subblocks.push_back({static_cast<uint32_t>(BlockStartHostCode - CodeData.BlockEntry),
                                      static_cast<uint32_t>(GetCursorAddress<uint8_t*>() - BlockStartHostCode)});
    }
  }

  // Make sure last branch is generated. It certainly can't be eliminated here.
  if (PendingTargetLabel) {
    b(PendingTargetLabel);
  }
  PendingTargetLabel = nullptr;

  // CodeSize not including the tail data.
  const uint64_t CodeOnlySize = GetCursorAddress<uint8_t*>() - CodeData.BlockBegin;

  // Add the JitCodeTail
  auto JITBlockTailLocation = GetCursorAddress<uint8_t*>();
  auto JITBlockTail = GetCursorAddress<JITCodeTail*>();
  CursorIncrement(sizeof(JITCodeTail));

  // Entries that live after the JITCodeTail.
  // These entries correlate JIT code regions with guest RIP regions.
  // Using these entries FEX is able to reconstruct the guest RIP accurately when an instruction cause a signal fault.
  // Packed using two variable length integer entries to ensure the size isn't too large.
  // These smaller sizes means that each entry is relative to each other instead of absolute offset from the start of the JIT block.
  // When reconstructing the RIP, each entry must be walked linearly and accumulated with the previous entries.
  // This is a trade-off between compression inside the JIT code space and execution time when reconstruction the RIP.
  // RIP reconstruction when faulting is less likely so we are requiring the accumulation.
  //
  // struct {
  //   // The Host PC offset from the previous entry.
  //   FEXCore::Utils::vl64 HostPCOffset;
  //   // How much to offset the RIP from the previous entry.
  //   FEXCore::Utils::vl64 GuestRIPOffset;
  // };

  auto JITRIPEntriesBegin = GetCursorAddress<uint8_t*>();

  // Put the block's RIP entry in the tail.
  // This will be used for RIP reconstruction in the future.
  // TODO: This needs to be a data RIP relocation once code caching works.
  //   Current relocation code doesn't support this feature yet.
  JITBlockTail->RIP = Entry;
  JITBlockTail->GuestSize = Size;
  JITBlockTail->SingleInst = SingleInst;
  JITBlockTail->SpinLockFutex = 0;

  auto JITRIPEntriesLocation = JITRIPEntriesBegin;

  {
    // Store the RIP entries.
    JITBlockTail->NumberOfRIPEntries = DebugData->GuestOpcodes.size();
    JITBlockTail->OffsetToRIPEntries = JITRIPEntriesBegin - JITBlockTailLocation;
    uintptr_t CurrentRIPOffset = 0;
    uint64_t CurrentPCOffset = 0;

    for (size_t i = 0; i < DebugData->GuestOpcodes.size(); i++) {
      const auto& GuestOpcode = DebugData->GuestOpcodes[i];
      int64_t HostPCOffset = GuestOpcode.HostEntryOffset - CurrentPCOffset;
      int64_t GuestRIPOffset = GuestOpcode.GuestEntryOffset - CurrentRIPOffset;

      size_t Size = FEXCore::Utils::vl64::Encode(JITRIPEntriesLocation, HostPCOffset);
      JITRIPEntriesLocation += Size;

      Size = FEXCore::Utils::vl64::Encode(JITRIPEntriesLocation, GuestRIPOffset);
      JITRIPEntriesLocation += Size;

      CurrentPCOffset = GuestOpcode.HostEntryOffset;
      CurrentRIPOffset = GuestOpcode.GuestEntryOffset;
    }
  }

  CursorIncrement(JITRIPEntriesLocation - JITRIPEntriesBegin);
  Align();

  CodeHeader->OffsetToBlockTail = JITBlockTailLocation - CodeData.BlockBegin;

  CodeData.Size = GetCursorAddress<uint8_t*>() - CodeData.BlockBegin;

  JITBlockTail->Size = CodeData.Size;

  ClearICache(CodeData.BlockBegin, CodeOnlySize);

#ifdef VIXL_DISASSEMBLER
  if (Disassemble() & FEXCore::Config::Disassemble::STATS) {
    auto HeaderOp = IR->GetHeader();
    LOGMAN_THROW_A_FMT(HeaderOp->Header.Op == IR::OP_IRHEADER, "First op wasn't IRHeader");

    LogMan::Msg::IFmt("RIP: 0x{:x}", Entry);
    LogMan::Msg::IFmt("Guest Code instructions: {}", HeaderOp->NumHostInstructions);
    LogMan::Msg::IFmt("Host Code instructions: {}", CodeOnlySize >> 2);
    LogMan::Msg::IFmt("Blow-up Amt: {}x", double(CodeOnlySize >> 2) / double(HeaderOp->NumHostInstructions));
  }

  if (Disassemble() & FEXCore::Config::Disassemble::BLOCKS) {
    const auto DisasmEnd = reinterpret_cast<const vixl::aarch64::Instruction*>(JITBlockTailLocation);
    LogMan::Msg::IFmt("Disassemble Begin");
    for (auto PCToDecode = DisasmBegin; PCToDecode < DisasmEnd; PCToDecode += 4) {
      DisasmDecoder->Decode(PCToDecode);
      auto Output = Disasm->GetOutput();
      LogMan::Msg::IFmt("{}", Output);
    }
    LogMan::Msg::IFmt("Disassemble End");
  }
#endif

  if (DebugData) {
    DebugData->HostCodeSize = CodeData.Size;
    DebugData->Relocations = &Relocations;
  }

  this->IR = nullptr;

  return CodeData;
}

void Arm64JITCore::ResetStack() {
  if (SpillSlots == 0) {
    return;
  }

  const auto TotalSpillSlotsSize = SpillSlots * MaxSpillSlotSize;

  if (ARMEmitter::IsImmAddSub(TotalSpillSlotsSize)) {
    add(ARMEmitter::Size::i64Bit, ARMEmitter::Reg::rsp, ARMEmitter::Reg::rsp, TotalSpillSlotsSize);
  } else {
    // Too big to fit in a 12bit immediate
    LoadConstant(ARMEmitter::Size::i64Bit, TMP1, TotalSpillSlotsSize);
    add(ARMEmitter::Size::i64Bit, ARMEmitter::XReg::rsp, ARMEmitter::XReg::rsp, TMP1, ARMEmitter::ExtendedType::LSL_64, 0);
  }
}

fextl::unique_ptr<CPUBackend> CreateArm64JITCore(FEXCore::Context::ContextImpl* ctx, FEXCore::Core::InternalThreadState* Thread) {
  return fextl::make_unique<Arm64JITCore>(ctx, Thread);
}

} // namespace FEXCore::CPU
