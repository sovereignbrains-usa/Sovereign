#include "log_ring.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace sovereign::service {

std::string_view TruncateUtf8(std::string_view text, std::size_t maxBytes) {
  if (text.size() <= maxBytes) {
    return text;
  }
  std::size_t end = maxBytes;
  // Step back over continuation bytes (10xxxxxx) so the cut lands before the
  // lead byte of a multi-byte sequence rather than inside it.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  return text.substr(0, end);
}

LogRing::LogRing(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

void LogRing::Append(LogLevel level, std::string_view message) {
  const std::scoped_lock lock(mutex_);
  PushLocked(level, message);
}

void LogRing::PushLocked(LogLevel level, std::string_view message) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  LogEntry entry{0, level, std::string(TruncateUtf8(message, kMaxMessageBytes)),
                 std::chrono::duration_cast<std::chrono::milliseconds>(now).count()};
  entry.seq = ++lastSeq_;
  if (entries_.size() == capacity_) {
    entries_.pop_front();
  }
  entries_.push_back(std::move(entry));
}

namespace {

// The line without its numbers: "ERROR[0489] ... 5.45.215.23:443" and
// "ERROR[0494] ... 5.45.215.23:443" are the same line.
std::string CollapseKey(std::string_view message) {
  std::string key;
  key.reserve(message.size());
  for (const char c : message) {
    if (c < '0' || c > '9') {
      key.push_back(c);
    }
  }
  return key;
}

}  // namespace

void LogRing::AppendCollapsed(LogLevel level, std::string_view message) {
  const std::string key = CollapseKey(message);
  const std::scoped_lock lock(mutex_);
  if (lastSeq_ > 0 && level == collapseLevel_ && key == collapseKey_) {
    ++collapsed_;
    return;
  }
  if (collapsed_ > 0) {
    PushLocked(collapseLevel_, "(the line above repeated " + std::to_string(collapsed_) + " more times)");
    collapsed_ = 0;
  }
  collapseKey_ = key;
  collapseLevel_ = level;
  PushLocked(level, message);
}

std::vector<LogEntry> LogRing::Since(std::uint64_t afterSeq) const {
  const std::scoped_lock lock(mutex_);
  // Sequence numbers are contiguous inside the ring, so the first wanted
  // entry's position follows from the oldest one's seq — no search needed.
  std::vector<LogEntry> result;
  if (entries_.empty() || afterSeq >= lastSeq_) {
    return result;
  }
  const std::uint64_t firstSeq = entries_.front().seq;
  const std::size_t skip =
      afterSeq < firstSeq ? 0 : static_cast<std::size_t>(afterSeq - firstSeq + 1);
  result.assign(entries_.begin() + static_cast<std::ptrdiff_t>(skip), entries_.end());
  return result;
}

LogLevel EffectiveLevel(LogLevel level, std::string_view message) {
  if (level == LogLevel::Error && message.find("canceled by remote with error code 0") != std::string_view::npos &&
      message.find("closed: stream ") != std::string_view::npos) {
    return LogLevel::Debug;
  }
  return level;
}

std::uint64_t LogRing::LastSeq() const {
  const std::scoped_lock lock(mutex_);
  return lastSeq_;
}

}  // namespace sovereign::service
