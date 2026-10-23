// Prepared protected-resource adapter. Modified 2026-10-23.
// A verifier must produce this input after hashing the ORIGINAL native XEX,
// every FINAL prepared protected file and receipt and independently rebuilding
// the 66-entry resource. This header alone is NOT a readiness/receipt verifier.
#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>
#include "native_protected_verifier.hpp"

namespace fm4_plus_proposal {
// Call only once in OnPostLoadXexImage, BEFORE any guest module is launched.
// No public SDK ABI, generated PPC function, entry point or disk-XEX change.
// Returns the guest allocation for logging/one-load ownership. Once committed,
// retain until the owning Runtime/Memory is destroyed; OnShutdown occurs before
// all possible guest readers stop and must NOT eagerly SystemHeapFree this.
inline uint32_t InstallResourceBeforeGuestStart(
    rex::system::UserModule& module, rex::memory::Memory& memory,
    const VerifiedResource& proof) {
  constexpr uint32_t kOldAddress=0x82EA0000, kOldSize=0x5771;
  const auto bytes=proof.bytes();
  if (module.entry_point()!=0x8275F9F0 || bytes.empty() ||
      bytes.size()>(1u<<20) || !module.guest_xex_header())
    throw std::runtime_error("PFP native resource/input is not qualified");
  rex::xex2_opt_resource_info* directory=nullptr;
  if (module.GetOptHeader(rex::XEX_HEADER_RESOURCE_INFO,&directory)!=rex::X_STATUS(0) || !directory)
    throw std::runtime_error("PFP native resource directory unavailable");
  const auto* header=reinterpret_cast<const uint8_t*>(module.xex_header());
  const uint32_t header_size=module.xex_header()->header_size;
  const auto* begin=reinterpret_cast<const uint8_t*>(directory);
  if (begin<header || size_t(begin-header)>header_size ||
      header_size-size_t(begin-header)<4 || directory->size<4 ||
      (directory->size-4)%sizeof(rex::xex2_resource) ||
      directory->size>header_size-size_t(begin-header))
    throw std::runtime_error("PFP resource directory bounds invalid");
  rex::xex2_resource* selected=nullptr;
  for(uint32_t i=0;i<(directory->size-4)/sizeof(rex::xex2_resource);++i) {
    auto& entry=directory->resources[i];
    if(std::memcmp(entry.name,"0D163575",8)==0) {
      if(selected)throw std::runtime_error("Duplicate PFP protected resource");
      selected=&entry;
    }
  }
  if(!selected || selected->address!=kOldAddress || selected->size!=kOldSize)
    throw std::runtime_error("PFP original protected resource mismatch");
  const size_t offset=reinterpret_cast<const uint8_t*>(selected)-header;
  auto* guest=reinterpret_cast<rex::xex2_resource*>(
      memory.TranslateVirtual(module.guest_xex_header())+offset);
  if(std::memcmp(selected,guest,sizeof(*selected))!=0)
    throw std::runtime_error("PFP host/guest directory copies disagree");
  // All checks and throwing work occur before either directory is changed.
  const uint32_t address=memory.SystemHeapAlloc(uint32_t(bytes.size()),16);
  if(!address)throw std::runtime_error("PFP guest resource allocation failed");
  const uint64_t end=uint64_t(address)+bytes.size();
  if((address&15) || end>(uint64_t(1)<<32) ||
      (address<0x82F50000 && end>0x82000000)) {
    memory.SystemHeapFree(address);
    throw std::runtime_error("PFP allocation overlaps native module");
  }
  std::memcpy(memory.TranslateVirtual(address),bytes.data(),bytes.size());
  // Guest is not running: these two directories are a pre-launch transaction,
  // not an atomic hot patch. No allocation, callbacks or exception after here.
  selected->address=address;selected->size=uint32_t(bytes.size());
  guest->address=address;guest->size=uint32_t(bytes.size());
  return address;
}
} // namespace fm4_plus_proposal
