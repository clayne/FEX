// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/memory.h>

namespace FEXCore {
struct HostFeatures;
} // namespace FEXCore

namespace FEXCore::Utils {
class IntrusivePooledAllocator;
}

namespace FEXCore::IR {
class Pass;
class RegisterAllocationPass;

fextl::unique_ptr<FEXCore::IR::Pass> CreateConstProp(bool SupportsTSOImm9);
fextl::unique_ptr<FEXCore::IR::Pass> CreateDeadFlagCalculationEliminination();
fextl::unique_ptr<FEXCore::IR::RegisterAllocationPass> CreateRegisterAllocationPass();
fextl::unique_ptr<FEXCore::IR::Pass> CreateX87StackOptimizationPass(const FEXCore::HostFeatures&, OpSize GPROpSize);

namespace Validation {
  fextl::unique_ptr<FEXCore::IR::Pass> CreateIRValidation();
} // namespace Validation

namespace Debug {
  fextl::unique_ptr<FEXCore::IR::Pass> CreateIRDumper();
}
} // namespace FEXCore::IR
