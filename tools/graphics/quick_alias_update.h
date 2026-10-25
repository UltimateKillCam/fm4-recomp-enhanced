// Exact logical interval metadata replacement. GPL-3.0-only.
#pragma once
#include <iterator>
#include <utility>
namespace rex::graphics::d3d12::native_rt {
template <typename Map, typename Range, typename EqualContent>
bool TryAssignExactInterval(Map& ranges, const Range& replacement,
                            EqualContent equal_content) {
  if (replacement.first >= replacement.end) return true;
  auto it = ranges.find({replacement.domain, replacement.first});
  if (it == ranges.end() || it->second.end != replacement.end) return false;
  it->second = replacement;
  if (it != ranges.begin()) {
    auto before = std::prev(it);
    if (before->second.end == it->second.first && equal_content(before->second, it->second)) {
      before->second.end = it->second.end;
      ranges.erase(it);
      it = before;
    }
  }
  auto after = std::next(it);
  if (after != ranges.end() && it->second.end == after->second.first &&
      equal_content(it->second, after->second)) {
    it->second.end = after->second.end;
    ranges.erase(after);
  }
  return true;
}
}  // namespace rex::graphics::d3d12::native_rt
