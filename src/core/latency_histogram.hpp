#pragma once

// Latency histogram with exact 1 ns buckets up to kExactLimitNs.
//
// Values at or above the limit are counted but not bucketed; a percentile that
// falls in that region is reported as the max seen. Recording is a few adds and
// a compare: cheap enough to call once per message outside the timed region.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ttt::core {

class LatencyHistogram {
public:
  static constexpr uint64_t kExactLimitNs = 1 << 16; // 65,536 ns

  LatencyHistogram() : buckets_(kExactLimitNs, 0) {}

  void record(uint64_t ns) {
    ++count_;
    sum_ += ns;
    min_ = std::min(min_, ns);
    max_ = std::max(max_, ns);
    if (ns < kExactLimitNs) {
      ++buckets_[ns];
    } else {
      ++overflow_;
    }
  }

  void merge(const LatencyHistogram& other) {
    if (other.count_ == 0) return;
    for (uint64_t i = 0; i < kExactLimitNs; ++i) buckets_[i] += other.buckets_[i];
    count_ += other.count_;
    sum_ += other.sum_;
    overflow_ += other.overflow_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }

  // Smallest recorded value v such that at least q of all samples are <= v.
  // q in [0, 1]: 0.5 = p50, 0.99 = p99, 0.999 = p99.9.
  uint64_t percentile(double q) const {
    if (count_ == 0) return 0;
    const auto rank = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::ceil(q * static_cast<double>(count_))));
    uint64_t seen = 0;
    for (uint64_t ns = 0; ns < kExactLimitNs; ++ns) {
      seen += buckets_[ns];
      if (seen >= rank) return ns;
    }
    return max_; // in the overflow region
  }

  uint64_t count() const { return count_; }
  uint64_t overflow() const { return overflow_; }
  uint64_t min() const { return count_ ? min_ : 0; }
  uint64_t max() const { return max_; }
  double mean() const { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0; }

private:
  std::vector<uint64_t> buckets_;
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  uint64_t overflow_ = 0;
  uint64_t min_ = UINT64_MAX;
  uint64_t max_ = 0;
};

} // namespace ttt::core
