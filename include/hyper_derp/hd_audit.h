/// @file hd_audit.h
/// @brief Routing-policy decision audit log.
///
/// Decisions made by HdResolve are appended to a
/// lock-free ring buffer. The ring is read by a
/// background flusher thread that writes LD-JSON lines
/// to a rotated file (Phase 4) and served live to the
/// metrics server.
///
/// Phase 2: ring + in-memory query. File sink and
/// rotation ship in Phase 4.

#ifndef INCLUDE_HYPER_DERP_HD_AUDIT_H_
#define INCLUDE_HYPER_DERP_HD_AUDIT_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/hd_resolver.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Ring capacity (power of two).
inline constexpr int kHdAuditRingSize = 4096;

/// One audit record. POD on purpose: the writer fills a
/// slot without allocating.
struct HdAuditRecord {
  uint64_t ts_ns = 0;
  Key client_key{};
  Key target_key{};
  HdIntent client_intent = HdIntent::kPreferDirect;
  HdConnMode mode = HdConnMode::kDenied;
  HdDenyReason deny_reason = HdDenyReason::kNone;
  uint8_t sub_reason = 0;
  bool allow_upgrade = true;
  bool allow_downgrade = true;
  uint8_t layer_allowed[5] = {0};
};

/// Lock-free single-producer-multiple-consumer ring.
/// Writes come from the server's control plane thread
/// (single producer). Readers are the metrics HTTP
/// server (may be multiple, all read-only) and the
/// background flusher (single reader — kept separate so
/// it doesn't race with HTTP snapshots).
struct HdAuditRing {
  HdAuditRecord slots[kHdAuditRingSize]{};
  // `write_idx` is the sequence number of the next slot
  // to write. Monotonic, wraps after 2^64 writes (never
  // in practice).
  std::atomic<uint64_t> write_idx{0};
  // Next sequence number the flusher has not yet sent to
  // disk. Lags behind `write_idx`. Under normal load the
  // gap is small; on startup with no file sink it equals
  // write_idx and no flushing happens.
  std::atomic<uint64_t> flush_idx{0};
};

/// @brief Initialize the audit ring. Call once at
///   server start.
void HdAuditInit(HdAuditRing* ring);

/// @brief Appends a decision to the ring.
///
/// Non-blocking. On ring overflow, oldest entries are
/// silently overwritten (the ring is best-effort;
/// Phase 4's file sink is authoritative).
void HdAuditRecordDecision(HdAuditRing* ring,
                           const Key& client_key,
                           const Key& target_key,
                           const HdClientView& client,
                           const HdDecision& decision);

/// @brief Copies up to `max_out` most-recent records
///   into `out`, newest first.
/// @returns Number of records copied.
int HdAuditSnapshot(const HdAuditRing* ring,
                    HdAuditRecord* out,
                    int max_out);

/// @brief Serializes a record to a single-line JSON
///   string matching the schema in HD_ROUTING_POLICY.
std::string HdAuditToJson(const HdAuditRecord& rec);

/// @brief Background flusher state. Owns the file fd +
///   rotation counters + dedicated thread.
struct HdAuditFlusher {
  HdAuditRing* ring = nullptr;
  std::string path;
  uint64_t max_bytes = 0;
  int keep = 10;
  std::thread thread;
  std::atomic<int> running{0};
  int fd = -1;
  uint64_t current_bytes = 0;
};

/// @brief Starts the background flusher if `path` is
///   non-empty. Silently no-ops on empty path.
/// @param flusher State struct (owned by caller).
/// @param ring Audit ring to drain from.
/// @param path Target file path (appended with O_APPEND).
/// @param max_bytes Rotate when the active file reaches
///   this size. 0 disables rotation.
/// @param keep Number of rotated files to keep
///   (audit.log.1 .. audit.log.N). Excess is unlinked.
void HdAuditFlusherStart(HdAuditFlusher* flusher,
                         HdAuditRing* ring,
                         const std::string& path,
                         uint64_t max_bytes,
                         int keep);

/// @brief Stops and joins the flusher thread, flushing
///   any remaining buffered records.
void HdAuditFlusherStop(HdAuditFlusher* flusher);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_AUDIT_H_
