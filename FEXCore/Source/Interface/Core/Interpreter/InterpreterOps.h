// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/IR/IR.h>

namespace FEXCore::IR {
class IRListView;
struct IROp_Header;
} // namespace FEXCore::IR

namespace FEXCore::CPU {
enum FallbackABI {
  FABI_F80_I16_F32_PTR,
  FABI_F80_I16_F64_PTR,
  FABI_F80_I16_I16_PTR,
  FABI_F80_I16_I32_PTR,
  FABI_F32_I16_F80_PTR,
  FABI_F64_I16_F80_PTR,
  FABI_F64_I16_F64_PTR,
  FABI_F64_I16_F64_F64_PTR,
  FABI_I16_I16_F80_PTR,
  FABI_I32_I16_F80_PTR,
  FABI_I64_I16_F80_PTR,
  FABI_I64_I16_F80_F80_PTR,
  FABI_F80_I16_F80_PTR,
  FABI_F80_I16_F80_F80_PTR,
  FABI_F80x2_I16_F80_PTR,
  FABI_F64x2_I16_F64_PTR,
  FABI_I32_I64_I64_V128_V128_I16,
  FABI_I32_V128_V128_I16,
  FABI_UNKNOWN,
};
struct FallbackInfo {
  FallbackABI ABI;
  FEXCore::Core::FallbackHandlerIndex HandlerIndex;
};

class InterpreterOps {
public:
  static void FillFallbackIndexPointers(Core::FallbackABIInfo* Info, uint64_t* ABIHandlers);
  static bool GetFallbackHandler(const IR::IROp_Header* IROp, FallbackInfo* Info);
};
} // namespace FEXCore::CPU
