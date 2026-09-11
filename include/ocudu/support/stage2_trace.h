// Opt-in FYP Stage 2 tracing. No E2 protocol or allocation-policy changes.
#pragma once
#include "ocudu/ocudulog/ocudulog.h"
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace ocudu { namespace stage2 {
inline bool enabled() {
  static const bool value = [] { const char* p = std::getenv("FYP_STAGE2_TRACE"); return p && std::string(p) == "1"; }();
  return value;
}
inline uint64_t now_ns(clockid_t clock = CLOCK_REALTIME) {
  timespec ts{};
  clock_gettime(clock, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ULL + uint64_t(ts.tv_nsec);
}
struct window {
  uint64_t start_ns = 0, end_ns = 0, start_mono_ns = 0, end_mono_ns = 0;
};
// The first interval is intentionally incomplete. Never infer a boundary from a configured timer.
inline window close_window(window& previous) {
  if (!enabled()) return {};
  window result{previous.end_ns, now_ns(), previous.end_mono_ns, now_ns(CLOCK_MONOTONIC)};
  previous = result;
  return result;
}
inline uint64_t next_id() {
  static std::atomic<uint64_t> id{0};
  return ++id;
}
inline void emit(const std::string& fields) {
  if (!enabled()) return;
  static auto& log = []() -> ocudulog::basic_logger& {
    auto& l = ocudulog::fetch_basic_logger("STAGE2");
    l.set_level(ocudulog::basic_levels::info);
    return l;
  }();
  // ocudulog queues asynchronously; capture time before queueing, not from the log prefix.
  static std::atomic<uint64_t> sequence{0};
  const auto event_seq = ++sequence;
  const auto wall = now_ns();
  const auto mono = now_ns(CLOCK_MONOTONIC);
  log.info("STAGE2 {{\"schema\":1,\"wall_ns\":{},\"mono_ns\":{},\"event_seq\":{},{}}}", wall, mono, event_seq, fields);
}
inline std::vector<std::string>& sources() { static thread_local std::vector<std::string> value; return value; }
inline bool& collecting() { static thread_local bool value = false; return value; }
struct collection_scope {
  collection_scope() { if(enabled()) { sources().clear(); collecting() = true; } }
  ~collection_scope() { collecting() = false; sources().clear(); }
};
inline void source(const char* metric, int64_t ue, const window& w, uint64_t seq) {
  if (!enabled() || !collecting()) return;
  sources().push_back("{\"metric\":\"" + std::string(metric) + "\",\"ue\":" + std::to_string(ue) +
    ",\"seq\":" + std::to_string(seq) + ",\"start_ns\":" + std::to_string(w.start_ns) +
    ",\"end_ns\":" + std::to_string(w.end_ns) + ",\"start_mono_ns\":" + std::to_string(w.start_mono_ns) +
    ",\"end_mono_ns\":" + std::to_string(w.end_mono_ns) + "}");
}
template<typename A, typename B> inline std::string fingerprint(const A& a, const B& b) {
  uint64_t hash = 14695981039346656037ULL;
  for(auto x : a) { hash ^= uint8_t(x); hash *= 1099511628211ULL; }
  // Length boundary prevents ambiguity between header/message concatenations.
  for(unsigned i=0;i<8;++i) { hash ^= uint8_t(uint64_t(a.length()) >> (i*8)); hash *= 1099511628211ULL; }
  for(auto x : b) { hash ^= uint8_t(x); hash *= 1099511628211ULL; }
  return std::to_string(hash);
}
template<typename A, typename B> inline void report(const A& header, const B& message) {
  if(!enabled()) return;
  std::string fields = "\"event\":\"kpm_source\",\"report\":\"" + fingerprint(header,message) + "\",\"sources\":[";
  bool first=true;
  for(const auto& s : sources()) { if(!first) fields+=","; first=false; fields+=s; }
  emit(fields+"]");
}
} } // namespace ocudu::stage2
