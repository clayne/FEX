// SPDX-License-Identifier: MIT
#include <FEXCore/Core/Thunks.h>
#include <FEXCore/fextl/memory.h>

namespace FEX::HLE {
struct ThreadStateObject;

class ThunkHandler : public FEXCore::ThunkHandler {
public:
  virtual void RegisterTLSState(FEXCore::Context::Context* CTX, FEX::HLE::ThreadStateObject* ThreadObject) = 0;
  /**
   * @brief Allows the frontend to register its own thunk handlers independent of what is controlled in the backend.
   *
   * @param CTX A valid non-null context instance.
   * @param Definitions A vector of thunk definitions that the frontend controls
   */
  virtual void AppendThunkDefinitions(std::span<const FEXCore::IR::ThunkDefinition> Definitions) = 0;
};
fextl::unique_ptr<ThunkHandler> CreateThunkHandler();
} // namespace FEX::HLE
