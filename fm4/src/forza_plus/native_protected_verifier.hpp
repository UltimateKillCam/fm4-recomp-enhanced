// Private Windows CPU verifier. Independent implementation, no game/GPU work.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fm4_plus_proposal {
using Digest=std::array<uint8_t,32>;
inline void Require(bool condition,const char* error) {if(!condition)throw std::runtime_error(error);}
inline void Require(bool condition,const std::string& error) {if(!condition)throw std::runtime_error(error);}
class Hash {
  BCRYPT_ALG_HANDLE algorithm_{};
  BCRYPT_HASH_HANDLE hash_{};
  std::vector<uint8_t> object_;
 public:
  Hash() {
    Require(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 provider failed");
    DWORD length=0,result=0;
    if(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&result,0)<0) {
      BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;throw std::runtime_error("SHA256 object property failed");
    }
    try {object_.resize(length);}catch(...) {BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;throw;}
    if(BCryptCreateHash(algorithm_,&hash_,object_.data(),length,nullptr,0,0)<0) {
      BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;throw std::runtime_error("SHA256 hash failed");
    }
  }
  Hash(const Hash&)=delete;
  ~Hash(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
  void Add(std::span<const uint8_t> bytes) {
    Require(bytes.size()<=std::numeric_limits<ULONG>::max(),"SHA256 input too large");
    Require(BCryptHashData(hash_,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)>=0,"SHA256 update failed");
  }
  Digest Finish(){Digest out{};Require(BCryptFinishHash(hash_,out.data(),ULONG(out.size()),0)>=0,"SHA256 finish failed");return out;}
};
inline Digest Sha(std::span<const uint8_t> bytes){Hash h;h.Add(bytes);return h.Finish();}
inline std::string Hex(const Digest& digest) {
  constexpr char digits[]="0123456789abcdef";std::string result;result.reserve(64);
  for(auto byte:digest){result.push_back(digits[byte>>4]);result.push_back(digits[byte&15]);}return result;
}
struct CloseHandleOwner {void operator()(void* value)const {if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
using OwnedHandle=std::unique_ptr<void,CloseHandleOwner>;

// Extended-length form for Win32 calls, so deep content paths work regardless of MAX_PATH.
inline std::wstring LongPath(const std::filesystem::path& path) {
  const std::wstring value=std::filesystem::path(path).make_preferred().native();
  if(value.rfind(LR"(\\?\)",0)==0)return value;
  if(value.rfind(LR"(\\)",0)==0)return LR"(\\?\UNC\)"+value.substr(2);
  return LR"(\\?\)"+value;
}
// Plain form for path walking: strips an extended-length prefix added by LongPath.
inline std::filesystem::path PlainPath(const std::filesystem::path& path) {
  const std::wstring value=std::filesystem::path(path).make_preferred().native();
  if(value.rfind(LR"(\\?\UNC\)",0)==0)return std::filesystem::path(LR"(\\)"+value.substr(8));
  if(value.rfind(LR"(\\?\)",0)==0)return std::filesystem::path(value.substr(4));
  return std::filesystem::path(value);
}
inline void RejectReparse(const std::filesystem::path& input) {
  const auto path=PlainPath(input);
  Require(path.is_absolute(),"Native proof paths must be absolute");
  std::filesystem::path ancestor=path.root_path();
  for(const auto& part:path.relative_path()) {
    Require(part!=L".." && part!=L".","Unnormalized native proof path");
    ancestor/=part;
    const DWORD flags=GetFileAttributesW(LongPath(ancestor).c_str());
    if(flags==INVALID_FILE_ATTRIBUTES || (flags&FILE_ATTRIBUTE_REPARSE_POINT)) {
      const auto text=ancestor.u8string();const std::string name(reinterpret_cast<const char*>(text.data()),text.size());
      Require(flags!=INVALID_FILE_ATTRIBUTES,"Missing native proof path: "+name);
      Require(false,"Native proof path has a reparse point: "+name);
    }
  }
}
inline OwnedHandle OpenStable(const std::filesystem::path& path) {
  RejectReparse(path);
  HANDLE raw=CreateFileW(LongPath(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  Require(raw!=INVALID_HANDLE_VALUE,"Cannot open stable native proof file");
  OwnedHandle result(raw);BY_HANDLE_FILE_INFORMATION info{};
  Require(GetFileInformationByHandle(raw,&info)!=0,"Cannot stat native proof handle");
  Require(!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),"Proof handle is not an ordinary file");
  return result;
}
inline uint64_t FileSize(HANDLE file){LARGE_INTEGER size{};Require(GetFileSizeEx(file,&size)!=0 && size.QuadPart>=0,"Invalid native file size");return uint64_t(size.QuadPart);}
inline void ReadExact(HANDLE file,std::span<uint8_t> bytes) {
  while(!bytes.empty()) {
    DWORD count=0;Require(ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)!=0 && count>0,"Short native proof read");
    bytes=bytes.subspan(count);
  }
}
inline Digest HashFile(HANDLE file) {
  Hash hash;std::array<uint8_t,32768> block{};uint64_t remaining=FileSize(file);
  while(remaining){size_t amount=size_t(std::min<uint64_t>(remaining,block.size()));ReadExact(file,std::span(block).first(amount));hash.Add(std::span(block).first(amount));remaining-=amount;}
  return hash.Finish();
}
inline uint32_t Be32(std::span<const uint8_t> bytes,size_t at) {
  Require(at<=bytes.size() && bytes.size()-at>=4,"Short signature field");
  return (uint32_t(bytes[at])<<24)|(uint32_t(bytes[at+1])<<16)|(uint32_t(bytes[at+2])<<8)|bytes[at+3];
}
inline void Put32(std::vector<uint8_t>& bytes,uint32_t value) {
  for(int shift=24;shift>=0;shift-=8)bytes.push_back(uint8_t(value>>shift));
}
inline std::filesystem::path Relative(const std::string& name) {
  Require(!name.empty(),"Empty signature path");std::filesystem::path result;std::string part;
  auto append=[&](){Require(!part.empty() && part!="." && part!=".." && part.back()!='.' && part.back()!=' ',"Unsafe signature segment");result/=part;part.clear();};
  for(char byte:name) {
    if(byte=='/' || byte=='\\'){append();continue;}
    Require(byte>=32 && byte<127 && byte!=':' && byte!='*' && byte!='?' && byte!='"' && byte!='<' && byte!='>' && byte!='|',"Unsafe signature path byte");part.push_back(byte);
  }
  append();Require(!result.is_absolute(),"Absolute signature path");return result;
}

class VerifiedResource {
  std::vector<uint8_t> bytes_;
  std::vector<OwnedHandle> locked_files_;
  VerifiedResource()=default;
  friend VerifiedResource VerifyProtectedFiles(const std::filesystem::path&,const std::filesystem::path&,std::span<const uint8_t>);
 public:
  VerifiedResource(VerifiedResource&&)=default;
  VerifiedResource& operator=(VerifiedResource&&)=default;
  VerifiedResource(const VerifiedResource&)=delete;
  std::span<const uint8_t> bytes() const{return bytes_;}
  // Retain this object in app-owned state through module launch. Its handles
  // deny writes/deletes to all protected inputs and the exact original XEX.
};

inline VerifiedResource VerifyProtectedFiles(const std::filesystem::path& root,
    const std::filesystem::path& expected_blob,std::span<const uint8_t> original_table) {
  Require(Hex(Sha(original_table))=="9948f4053e2ebf91b3c998cb5dd49d5658f50cb9631a354ea86e2d4c5a512c98","Original loaded resource hash mismatch");
  Require(original_table.size()==22385 && Be32(original_table,0)==32768 && Be32(original_table,4)==66,"Original signature version mismatch");
  RejectReparse(root);VerifiedResource proof;proof.locked_files_.reserve(68);
  auto xex=OpenStable(root/L"default.xex");
  Require(Hex(HashFile(xex.get()))=="b34dfb88978819cccda6dafd755e7139970a58fbf6707dfbcba06846857f52bd","Prepared executable differs from original native input");
  proof.locked_files_.push_back(std::move(xex));
  auto expected=OpenStable(expected_blob);uint64_t expected_size=FileSize(expected.get());
  Require(expected_size>0 && expected_size<=(1u<<20),"Expected resource size invalid");
  std::vector<uint8_t> expected_bytes(size_t(expected_size),uint8_t{});ReadExact(expected.get(),expected_bytes);
  proof.locked_files_.push_back(std::move(expected));
  constexpr size_t start=8+66*8;std::vector<uint8_t> index,payload;std::set<std::string> unique;
  for(size_t i=0;i<66;++i) {
    const size_t offset=Be32(original_table,8+i*8),position=start+offset;
    Require(position<original_table.size(),"Original signature offset invalid");
    size_t end=position;while(end<original_table.size() && original_table[end])++end;
    Require(end<original_table.size(),"Original signature filename unterminated");
    std::string name(reinterpret_cast<const char*>(original_table.data()+position),end-position);
    std::string key=name;std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(c>='A'&&c<='Z'?c+32:c);});
    Require(unique.insert(key).second,"Duplicate signature filename");
    auto file=OpenStable(root/Relative(name));uint64_t size=FileSize(file.get());
    Require(size>0 && size<=0xffffffffu,"Unsupported protected file size");
    Require(payload.size()<=0xffffffffu,"Resource index overflow");Put32(index,uint32_t(payload.size()));Put32(index,uint32_t(size));
    payload.insert(payload.end(),name.begin(),name.end());payload.push_back(0);
    std::array<uint8_t,32768> block{};uint64_t remaining=size;
    while(remaining) {
      size_t amount=size_t(std::min<uint64_t>(remaining,block.size()));ReadExact(file.get(),std::span(block).first(amount));
      const auto hash=Sha(std::span(block).first(amount));payload.insert(payload.end(),hash.begin(),hash.end());remaining-=amount;
      Require(payload.size()+start<=(1u<<20),"Rebuilt resource exceeds allocation bound");
    }
    proof.locked_files_.push_back(std::move(file));
  }
  Put32(proof.bytes_,32768);Put32(proof.bytes_,66);proof.bytes_.insert(proof.bytes_.end(),index.begin(),index.end());proof.bytes_.insert(proof.bytes_.end(),payload.begin(),payload.end());
  Require(proof.bytes_==expected_bytes,"Prepared protected files differ from verified generation resource");
  return proof;
}
} // namespace fm4_plus_proposal
