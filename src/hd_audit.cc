/// @file hd_audit.cc
/// @brief Routing-policy decision audit log.

#include "hyper_derp/hd_audit.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <spdlog/spdlog.h>

#include "hyper_derp/key_format.h"

namespace hyper_derp {

namespace {

uint64_t NowNs() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

const char* IntentName(HdIntent i) {
  switch (i) {
    case HdIntent::kPreferDirect:
      return "prefer_direct";
    case HdIntent::kRequireDirect:
      return "require_direct";
    case HdIntent::kPreferRelay:
      return "prefer_relay";
    case HdIntent::kRequireRelay:
      return "require_relay";
  }
  return "unknown";
}

const char* ModeName(HdConnMode m) {
  switch (m) {
    case HdConnMode::kDenied:
      return "denied";
    case HdConnMode::kDirect:
      return "direct";
    case HdConnMode::kRelayed:
      return "relayed";
  }
  return "unknown";
}

const char* DenyName(HdDenyReason r) {
  switch (r) {
    case HdDenyReason::kNone:
      return "";
    case HdDenyReason::kPolicyForbids:
      return "policy_forbids";
    case HdDenyReason::kPairForbids:
      return "pair_forbids";
    case HdDenyReason::kPeerUnreachable:
      return "peer_unreachable";
    case HdDenyReason::kTargetUnresponsive:
      return "target_unresponsive";
    case HdDenyReason::kTooManyOpenConns:
      return "too_many_open_conns";
    case HdDenyReason::kFleetRoutingNotImplemented:
      return "fleet_routing_not_implemented";
    case HdDenyReason::kIntentConflict:
      return "intent_conflict";
    case HdDenyReason::kPeerOverride:
      return "peer_override";
    case HdDenyReason::kDirectCapExceeded:
      return "direct_cap_exceeded";
    case HdDenyReason::kRegionPolicyViolation:
      return "region_policy_violation";
    case HdDenyReason::kFederationDenied:
      return "federation_denied";
    case HdDenyReason::kNatIncompatible:
      return "nat_incompatible";
  }
  return "unknown";
}

void AppendAllowed(std::ostringstream& out,
                   uint8_t allowed) {
  bool first = true;
  out << '[';
  if (allowed & kModeDirect) {
    out << "\"direct\"";
    first = false;
  }
  if (allowed & kModeRelayed) {
    if (!first) out << ',';
    out << "\"relayed\"";
  }
  out << ']';
}

void FormatTs(uint64_t ns, char* buf, int buf_size) {
  time_t secs = static_cast<time_t>(ns / 1000000000ULL);
  struct tm tm_utc{};
  gmtime_r(&secs, &tm_utc);
  int ms = static_cast<int>(
      (ns % 1000000000ULL) / 1000000ULL);
  strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%S", &tm_utc);
  int len = static_cast<int>(std::strlen(buf));
  snprintf(buf + len, buf_size - len, ".%03dZ", ms);
}

}  // namespace

void HdAuditInit(HdAuditRing* ring) {
  ring->write_idx.store(0, std::memory_order_relaxed);
  for (auto& s : ring->slots) s = HdAuditRecord{};
}

void HdAuditRecordDecision(HdAuditRing* ring,
                           const Key& client_key,
                           const Key& target_key,
                           const HdClientView& client,
                           const HdDecision& decision) {
  uint64_t seq = ring->write_idx.fetch_add(
      1, std::memory_order_acq_rel);
  auto& slot = ring->slots[seq % kHdAuditRingSize];
  slot.ts_ns = NowNs();
  slot.client_key = client_key;
  slot.target_key = target_key;
  slot.client_intent = client.intent;
  slot.mode = decision.mode;
  slot.deny_reason = decision.deny_reason;
  slot.sub_reason = decision.sub_reason;
  slot.allow_upgrade = client.allow_upgrade;
  slot.allow_downgrade = client.allow_downgrade;
  for (int i = 0; i < 5; i++) {
    slot.layer_allowed[i] = decision.layer_allowed[i];
  }
}

int HdAuditSnapshot(const HdAuditRing* ring,
                    HdAuditRecord* out,
                    int max_out) {
  uint64_t write_idx =
      ring->write_idx.load(std::memory_order_acquire);
  int total = (write_idx < kHdAuditRingSize)
                  ? static_cast<int>(write_idx)
                  : kHdAuditRingSize;
  int n = total < max_out ? total : max_out;
  for (int i = 0; i < n; i++) {
    uint64_t seq = write_idx - 1 - i;
    out[i] = ring->slots[seq % kHdAuditRingSize];
  }
  return n;
}

std::string HdAuditToJson(const HdAuditRecord& rec) {
  char ts[64];
  FormatTs(rec.ts_ns, ts, sizeof(ts));
  std::ostringstream o;
  o << '{' << "\"ts\":\"" << ts << "\""
    << ",\"client\":\"" << KeyToCkString(rec.client_key)
    << "\""
    << ",\"target\":\"" << KeyToCkString(rec.target_key)
    << "\""
    << ",\"intent\":\""
    << IntentName(rec.client_intent) << "\""
    << ",\"decision\":\"" << ModeName(rec.mode) << "\"";
  if (rec.deny_reason != HdDenyReason::kNone) {
    o << ",\"reason\":\"" << DenyName(rec.deny_reason)
      << "\"";
  }
  o << ",\"allow_upgrade\":"
    << (rec.allow_upgrade ? "true" : "false")
    << ",\"allow_downgrade\":"
    << (rec.allow_downgrade ? "true" : "false");
  o << ",\"chain\":[";
  static const char* kNames[5] = {"fleet", "federation",
                                   "relay", "peer",
                                   "client"};
  for (int i = 0; i < 5; i++) {
    if (i > 0) o << ',';
    o << "{\"layer\":\"" << kNames[i] << "\",\"allowed\":";
    AppendAllowed(o, rec.layer_allowed[i]);
    o << '}';
  }
  o << ']' << '}';
  return o.str();
}

// -- File sink + rotation -----------------------------

namespace {

void RotateFiles(HdAuditFlusher* f) {
  // Unlink the oldest, shift 1..N-1 up by one, then
  // rename the active file to .1.
  std::string oldest =
      f->path + "." + std::to_string(f->keep);
  ::unlink(oldest.c_str());
  for (int i = f->keep - 1; i >= 1; i--) {
    std::string from =
        f->path + "." + std::to_string(i);
    std::string to =
        f->path + "." + std::to_string(i + 1);
    ::rename(from.c_str(), to.c_str());
  }
  std::string first = f->path + ".1";
  ::rename(f->path.c_str(), first.c_str());
}

int OpenForAppend(const std::string& path,
                  uint64_t* current_bytes) {
  int fd = ::open(path.c_str(),
                  O_WRONLY | O_APPEND | O_CREAT,
                  0600);
  if (fd < 0) return -1;
  struct stat st{};
  if (::fstat(fd, &st) == 0) {
    *current_bytes = static_cast<uint64_t>(st.st_size);
  } else {
    *current_bytes = 0;
  }
  return fd;
}

bool WriteAllBytes(int fd, const char* data, size_t n) {
  size_t total = 0;
  while (total < n) {
    ssize_t w = ::write(fd, data + total, n - total);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    total += static_cast<size_t>(w);
  }
  return true;
}

void FlushOnce(HdAuditFlusher* f) {
  uint64_t write_idx =
      f->ring->write_idx.load(std::memory_order_acquire);
  uint64_t flush_idx = f->ring->flush_idx.load(
      std::memory_order_relaxed);
  // Drop any records we've already been lapped on.
  if (write_idx - flush_idx > kHdAuditRingSize) {
    flush_idx = write_idx - kHdAuditRingSize;
  }
  while (flush_idx < write_idx) {
    const HdAuditRecord& rec =
        f->ring->slots[flush_idx % kHdAuditRingSize];
    std::string line = HdAuditToJson(rec);
    line.push_back('\n');
    if (f->max_bytes > 0 && f->fd >= 0 &&
        f->current_bytes + line.size() >
            f->max_bytes) {
      ::close(f->fd);
      f->fd = -1;
      RotateFiles(f);
      f->fd = OpenForAppend(f->path, &f->current_bytes);
      if (f->fd < 0) break;
    }
    if (f->fd < 0) break;
    if (!WriteAllBytes(f->fd, line.data(),
                       line.size())) {
      spdlog::warn("audit log write failed: {}",
                   strerror(errno));
      break;
    }
    f->current_bytes += line.size();
    flush_idx++;
  }
  f->ring->flush_idx.store(flush_idx,
                           std::memory_order_release);
}

void FlushLoop(HdAuditFlusher* f) {
  while (f->running.load(std::memory_order_acquire)) {
    FlushOnce(f);
    // Cheap sleep; not latency-critical.
    usleep(100000);
  }
  // Final drain on stop.
  FlushOnce(f);
}

}  // namespace

void HdAuditFlusherStart(HdAuditFlusher* flusher,
                         HdAuditRing* ring,
                         const std::string& path,
                         uint64_t max_bytes,
                         int keep) {
  flusher->ring = ring;
  flusher->path = path;
  flusher->max_bytes = max_bytes;
  flusher->keep = keep > 0 ? keep : 10;
  flusher->fd = -1;
  flusher->current_bytes = 0;
  if (path.empty()) return;
  flusher->fd =
      OpenForAppend(path, &flusher->current_bytes);
  if (flusher->fd < 0) {
    spdlog::error("audit log open {}: {}", path,
                  strerror(errno));
    return;
  }
  // Skip any pre-existing ring contents: we only want
  // decisions made after startup.
  flusher->ring->flush_idx.store(
      flusher->ring->write_idx.load(
          std::memory_order_acquire),
      std::memory_order_release);
  flusher->running.store(1, std::memory_order_release);
  flusher->thread = std::thread(FlushLoop, flusher);
}

void HdAuditFlusherStop(HdAuditFlusher* flusher) {
  flusher->running.store(0, std::memory_order_release);
  if (flusher->thread.joinable()) {
    flusher->thread.join();
  }
  if (flusher->fd >= 0) {
    ::close(flusher->fd);
    flusher->fd = -1;
  }
}

}  // namespace hyper_derp
