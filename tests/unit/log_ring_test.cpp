#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "log_ring.h"

namespace {

using sovereign::service::LogLevel;
using sovereign::service::LogRing;
using sovereign::service::TruncateUtf8;

void TestTruncateUtf8() {
  CHECK(TruncateUtf8("hello", 10) == "hello");
  CHECK(TruncateUtf8("hello", 5) == "hello");
  CHECK(TruncateUtf8("hello", 3) == "hel");
  // "aж": 'a' + D0 B6. Cutting at 2 would split the Cyrillic letter.
  CHECK(TruncateUtf8("a\xD0\xB6", 2) == "a");
  CHECK(TruncateUtf8("a\xD0\xB6", 3) == "a\xD0\xB6");
  // "€" is E2 82 AC: any cut inside it falls back to before it.
  CHECK(TruncateUtf8("x\xE2\x82\xAC", 2) == "x");
  CHECK(TruncateUtf8("x\xE2\x82\xAC", 3) == "x");
  CHECK(TruncateUtf8("", 0).empty());
}

void TestEffectiveLevel() {
  using sovereign::service::EffectiveLevel;
  const std::string closed =
      "ERROR[10857] [733181642 1m9s] connection: connection upload closed: stream 548 canceled by remote with error code 0";
  CHECK(EffectiveLevel(LogLevel::Error, closed) == LogLevel::Debug);
  // A real code stays an error, and only errors are moved.
  CHECK(EffectiveLevel(LogLevel::Error, "connection upload closed: stream 5 canceled by remote with error code 268") ==
        LogLevel::Error);
  CHECK(EffectiveLevel(LogLevel::Warn, closed) == LogLevel::Warn);
  CHECK(EffectiveLevel(LogLevel::Error, "outbound/anytls[A]: failed to create session: EOF") == LogLevel::Error);
  CHECK(EffectiveLevel(LogLevel::Error, "network: missing default interface") == LogLevel::Error);
}

void TestAppendCollapsed() {
  LogRing ring(10);
  ring.AppendCollapsed(LogLevel::Error, "ERROR[0489] dial 5.45.215.23:443: timeout");
  ring.AppendCollapsed(LogLevel::Error, "ERROR[0494] dial 5.45.215.23:8443: timeout");
  ring.AppendCollapsed(LogLevel::Error, "ERROR[0501] dial 5.45.215.23:443: timeout");
  CHECK(ring.LastSeq() == 1);
  // A different line closes the run with one summary line.
  ring.AppendCollapsed(LogLevel::Error, "other failure");
  const auto all = ring.Since(0);
  CHECK(all.size() == 3);
  CHECK(all[1].message.find("2 more") != std::string::npos);
  CHECK(all[2].message == "other failure");
  // The same text at another level is another line.
  ring.AppendCollapsed(LogLevel::Warn, "other failure");
  CHECK(ring.Since(0).size() == 4);
}

void TestAppendAndSince() {  LogRing ring(3);
  CHECK(ring.LastSeq() == 0);
  CHECK(ring.Since(0).empty());

  for (int i = 1; i <= 5; ++i) {
    ring.Append(LogLevel::Info, "line " + std::to_string(i));
  }
  CHECK(ring.LastSeq() == 5);

  // Capacity 3: lines 1 and 2 were dropped, sequence numbers keep going.
  const auto all = ring.Since(0);
  CHECK(all.size() == 3);
  if (all.size() == 3) {
    CHECK(all[0].seq == 3 && all[0].message == "line 3");
    CHECK(all[2].seq == 5 && all[2].message == "line 5");
  }

  const auto tail = ring.Since(4);
  CHECK(tail.size() == 1 && tail[0].seq == 5);
  CHECK(ring.Since(5).empty());
  CHECK(ring.Since(100).empty());
}

void TestLongLineIsCut() {
  LogRing ring(1);
  ring.Append(LogLevel::Warn, std::string(LogRing::kMaxMessageBytes * 3, 'x'));
  const auto entries = ring.Since(0);
  CHECK(entries.size() == 1 && entries[0].message.size() == LogRing::kMaxMessageBytes);
  CHECK(entries.size() == 1 && entries[0].level == LogLevel::Warn);
}

// Writers race a reader; every reader snapshot must be a contiguous run of
// sequence numbers, and nothing may be lost from the total count.
void TestConcurrentAppend() {
  constexpr int kWriters = 4;
  constexpr int kLinesPerWriter = 2000;
  constexpr std::size_t kCapacity = 100;
  LogRing ring(kCapacity);

  bool contiguous = true;
  std::jthread reader([&](const std::stop_token& stop) {
    while (!stop.stop_requested()) {
      const auto snapshot = ring.Since(0);
      for (std::size_t i = 1; i < snapshot.size(); ++i) {
        if (snapshot[i].seq != snapshot[i - 1].seq + 1) {
          contiguous = false;
        }
      }
    }
  });

  {
    std::vector<std::jthread> writers;
    writers.reserve(kWriters);
    for (int w = 0; w < kWriters; ++w) {
      writers.emplace_back([&ring, w] {
        for (int i = 0; i < kLinesPerWriter; ++i) {
          ring.Append(LogLevel::Debug, "writer " + std::to_string(w));
        }
      });
    }
  }
  reader.request_stop();
  reader.join();

  constexpr std::uint64_t kTotal = static_cast<std::uint64_t>(kWriters) * kLinesPerWriter;
  CHECK(contiguous);
  CHECK(ring.LastSeq() == kTotal);
  const auto kept = ring.Since(0);
  CHECK(kept.size() == kCapacity);
  CHECK(!kept.empty() && kept.front().seq == kTotal - kCapacity + 1 && kept.back().seq == kTotal);
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape) — see the catch below
  try {
    TestTruncateUtf8();
    TestEffectiveLevel();
    TestAppendCollapsed();
    TestAppendAndSince();
    TestLongLineIsCut();
    TestConcurrentAppend();
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  if (sovereign::test::Failures() != 0) {
    return 1;
  }
  std::cout << "OK\n";
  return 0;
}
