// Coordinate local snapshots with guest file operations. GPL-3.0-only.
#pragma once
#include <mutex>
#include <shared_mutex>
namespace fm4::tools::io {
inline std::shared_mutex gate;
inline thread_local unsigned depth=0;
class Reader {
 public:
  explicit Reader(bool enabled) : enabled_(enabled), outer_(enabled && depth++==0) {
    if(outer_)gate.lock_shared();
  }
  Reader(const Reader&)=delete;
  ~Reader() {if(enabled_) {if(outer_)gate.unlock_shared();--depth;}}
 private:
  bool enabled_,outer_;
};
// Never queue an exclusive writer behind guest readers: another guest reader
// may already hold a runtime lock needed by one of the active readers.
inline auto TrySnapshot() {return std::unique_lock<std::shared_mutex>(gate,std::try_to_lock);}
}
