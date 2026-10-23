#pragma once
#include "native_protected_verifier.hpp"

namespace fm4_plus_proposal {
inline std::wstring EditionMutexBase(const std::filesystem::path& edition) {
  RejectReparse(edition);std::wstring value(32768,L'\0');
  const DWORD length=GetFullPathNameW(edition.c_str(),DWORD(value.size()),value.data(),nullptr);
  Require(length && length<value.size(),"Edition canonical path invalid");value.resize(length);
  while(value.size()>3 && value.back()==L'\\')value.pop_back();CharLowerBuffW(value.data(),DWORD(value.size()));
  const auto bytes=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()),value.size()*sizeof(wchar_t));
  const auto hash=Hex(Sha(bytes));return L"Local\\ProjectForza.Edition."+std::wstring(hash.begin(),hash.end());
}
class EditionGameLease {
  OwnedHandle game_,ready_,cancel_;bool owned_=false;
 public:
  EditionGameLease()=default;EditionGameLease(const EditionGameLease&)=delete;
  ~EditionGameLease(){if(owned_)ReleaseMutex(game_.get());}
  void Acquire(const std::filesystem::path& edition,const std::wstring& ready_name={},const std::wstring& cancel_name={}) {
    Require(!game_,"Edition game lease already initialized");
    const auto name=EditionMutexBase(edition)+L".Game";
    game_.reset(CreateMutexW(nullptr,FALSE,name.c_str()));Require(bool(game_),"Cannot create edition game mutex");
    const bool handoff=!ready_name.empty() || !cancel_name.empty();
    if(handoff) {
      Require(!ready_name.empty() && !cancel_name.empty(),"Incomplete launch handoff");
      ready_.reset(OpenEventW(EVENT_MODIFY_STATE,FALSE,ready_name.c_str()));
      cancel_.reset(OpenEventW(SYNCHRONIZE,FALSE,cancel_name.c_str()));
      Require(bool(ready_) && bool(cancel_),"Launcher handoff disappeared");
    }
    const DWORD waited=WaitForSingleObject(game_.get(),handoff?15000:0);
    Require(waited==WAIT_OBJECT_0 || waited==WAIT_ABANDONED,"Another process owns this edition");owned_=true;
    for(const auto* mode:{L"normal",L"hard",L"sandbox",L"vanilla"}) {
      const auto pending=edition/L".local/user/.fm4-plus"/mode/L".fm4-plus-pending.json";
      Require(!std::filesystem::exists(pending),"Pending edition content recovery required");
    }
    CheckCancellation();if(ready_)Require(SetEvent(ready_.get())!=0,"Cannot acknowledge launch lease");
  }
  void CheckCancellation()const {
    Require(!cancel_ || WaitForSingleObject(cancel_.get(),0)==WAIT_TIMEOUT,"Launcher cancelled this child before startup");
  }
};
// The OS closes this process-lifetime lease on exit; OnShutdown is not a full
// guest-drain guarantee, so releasing there would reopen a mutation race.
inline EditionGameLease& GameLease(){static EditionGameLease* lease=new EditionGameLease;return *lease;}
}
