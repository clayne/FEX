// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <tuple>

#include <FEXCore/fextl/map.h>
#include <FEXCore/Utils/SignalScopeGuards.h>

namespace FEXCore::IR {
struct AOTIRCacheEntry;
}

namespace FEX::HLE::VMATracking {
///// VMA (Virtual Memory Area) tracking /////

namespace SpecialDev {
  static constexpr uint64_t Anon = 0x1'0000'0000; // Anonymous shared mapping, id is incrementing allocation number
  static constexpr uint64_t SHM = 0x2'0000'0000;  // sys-v shm, id is shmid
};                                                // namespace SpecialDev

// Memory Resource ID
// An id that can be used to identify when shared mappings actually have the same backing storage
// when dev != SpecialDev::Anon, this is unique system wide
struct MRID {
  uint64_t dev; // kernel dev_t is actually 32-bits, we use the extra bits to track SpecialDevs
  uint64_t id;

  bool operator<(const MRID& other) const {
    return std::tie(dev, id) < std::tie(other.dev, other.id);
  }
};

struct VMAEntry;

// Used to all MAP_SHARED VMAs of a system resource.
struct MappedResource {
  using ContainerType = fextl::map<MRID, MappedResource>;

  FEXCore::IR::AOTIRCacheEntry* AOTIRCacheEntry;
  // Pointer to lowest memory range this file is mapped to
  VMAEntry* FirstVMA;
  uint64_t Length; // 0 if not fixed size
  ContainerType::iterator Iterator;
};

union VMAProt {
  struct {
    bool Readable   : 1;
    bool Writable   : 1;
    bool Executable : 1;
  };
  uint8_t All : 3;

  static VMAProt fromProt(int Prot);
  static VMAProt fromSHM(int SHMFlg);
};

struct VMAFlags {
  bool Shared : 1;

  static VMAFlags fromFlags(int Flags);
};

struct VMAEntry {
  MappedResource* Resource;

  // these are for intrusive linked list tracking, starting from Resource->FirstVMA and ordered by address
  VMAEntry* ResourcePrevVMA;
  VMAEntry* ResourceNextVMA;

  uint64_t Base;
  uint64_t Offset;
  uint64_t Length;

  VMAFlags Flags;
  VMAProt Prot;
};

struct VMATracking {
  // Held while reading/writing this struct
  FEXCore::ForkableSharedMutex Mutex;

  // Memory ranges indexed by page aligned starting address
  fextl::map<uint64_t, VMAEntry> VMAs;

  using VMACIterator = decltype(VMAs)::const_iterator;

  MappedResource::ContainerType MappedResources;

  // Mutex must be at least shared_locked before calling
  VMACIterator LookupVMAUnsafe(uint64_t GuestAddr) const;

  // Mutex must be unique_locked before calling
  void SetUnsafe(FEXCore::Context::Context* Ctx, MappedResource* MappedResource, uintptr_t Base, uintptr_t Offset, uintptr_t Length,
                 VMAFlags Flags, VMAProt Prot);

  // Mutex must be unique_locked before calling
  void ClearUnsafe(FEXCore::Context::Context* Ctx, uintptr_t Base, uintptr_t Length, MappedResource* PreservedMappedResource = nullptr);

  // Mutex must be unique_locked before calling
  void ChangeUnsafe(uintptr_t Base, uintptr_t Length, VMAProt Prot);

  // Mutex must be unique_locked before calling
  // Returns the Size fo the Shm or 0 if not found
  uintptr_t ClearShmUnsafe(FEXCore::Context::Context* Ctx, uintptr_t Base);
private:
  bool ListRemove(VMAEntry* Mapping);
  void ListReplace(VMAEntry* Mapping, VMAEntry* NewMapping);
  void ListInsertAfter(VMAEntry* Mapping, VMAEntry* NewMapping);
  void ListPrepend(MappedResource* Resource, VMAEntry* NewVMA);
  static void ListCheckVMALinks(VMAEntry* VMA);
};


} // namespace FEX::HLE::VMATracking
