// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/tiered_storage.h"

#include <mimalloc.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <variant>

#include "absl/cleanup/cleanup.h"
#include "absl/flags/internal/flag.h"
#include "base/flags.h"
#include "base/logging.h"
#include "server/common.h"
#include "server/db_slice.h"
#include "server/engine_shard_set.h"
#include "server/snapshot.h"
#include "server/table.h"
#include "server/tiering/common.h"
#include "server/tiering/op_manager.h"
#include "server/tiering/small_bins.h"
#include "server/tx_base.h"

ABSL_FLAG(bool, tiered_storage_cache_fetched, true,
          "WIP: Load results of offloaded reads to memory");

ABSL_FLAG(unsigned, tiered_storage_write_depth, 50,
          "Maximum number of concurrent stash requests issued by background offload");

namespace dfly {

using namespace std;
using namespace util;

using namespace tiering::literals;

using KeyRef = tiering::OpManager::KeyRef;

namespace {

bool OccupiesWholePages(size_t size) {
  return size >= TieredStorage::kMinOccupancySize;
}

// Stashed bins no longer have bin ids, so this sentinel is used to differentiate from regular reads
constexpr auto kFragmentedBin = tiering::SmallBins::kInvalidBin - 1;

// Called after setting new value in place of previous segment
void RecordDeleted(const PrimeValue& pv, size_t tiered_len, DbTableStats* stats) {
  stats->AddTypeMemoryUsage(pv.ObjType(), pv.MallocUsed());
  stats->tiered_entries--;
  stats->tiered_used_bytes -= tiered_len;
}

// Called before overriding value with segment
void RecordAdded(const PrimeValue& pv, size_t tiered_len, DbTableStats* stats) {
  stats->AddTypeMemoryUsage(pv.ObjType(), -pv.MallocUsed());
  stats->tiered_entries++;
  stats->tiered_used_bytes += tiered_len;
}

string DecodeString(bool is_raw, string_view str, PrimeValue decoder) {
  if (is_raw) {
    decoder.Materialize(str, true);
    string tmp;
    decoder.GetString(&tmp);
    return tmp;
  }
  return string{str};
}

}  // anonymous namespace

class TieredStorage::ShardOpManager : public tiering::OpManager {
  friend class TieredStorage;

 public:
  ShardOpManager(TieredStorage* ts, DbSlice* db_slice, size_t max_size)
      : tiering::OpManager{max_size}, ts_{ts}, db_slice_{db_slice} {
    cache_fetched_ = absl::GetFlag(FLAGS_tiered_storage_cache_fetched);
  }

  // Clear IO pending flag for entry
  void ClearIoPending(OpManager::KeyRef key) {
    if (auto pv = Find(key); pv) {
      pv->SetIoPending(false);
      stats_.total_cancels++;
    }
  }

  // Clear IO pending flag for all contained entries of bin
  void ClearIoPending(tiering::SmallBins::BinId id) {
    for (const auto& key : ts_->bins_->ReportStashAborted(id))
      ClearIoPending(key);
  }

  DbTableStats* GetDbTableStats(DbIndex dbid) {
    return db_slice_->MutableStats(dbid);
  }

 private:
  PrimeValue* Find(OpManager::KeyRef key) {
    // TODO: Get DbContext for transaction for correct dbid and time
    // Bypass all update and stat mechanisms
    auto it = db_slice_->GetDBTable(key.first)->prime.Find(key.second);
    return IsValid(it) ? &it->second : nullptr;
  }

  // Load all values from bin by their hashes
  void Defragment(tiering::DiskSegment segment, string_view value);

  void NotifyStashed(EntryId id, const io::Result<tiering::DiskSegment>& segment) override {
    if (!segment) {
      VLOG(1) << "Stash failed " << segment.error().message();
      visit([this](auto id) { ClearIoPending(id); }, id);
    } else {
      visit([this, segment](auto id) { SetExternal(id, *segment); }, id);
    }
  }

  bool NotifyFetched(EntryId id, string_view value, tiering::DiskSegment segment,
                     bool modified) override;

  bool NotifyDelete(tiering::DiskSegment segment) override;

  // Set value to be an in-memory type again. Update memory stats.
  void Upload(DbIndex dbid, string_view value, bool is_raw, size_t serialized_len, PrimeValue* pv) {
    DCHECK(!value.empty());

    pv->Materialize(value, is_raw);
    RecordDeleted(*pv, serialized_len, GetDbTableStats(dbid));
  }

  // Find entry by key in db_slice and store external segment in place of original value.
  // Update memory stats
  void SetExternal(OpManager::KeyRef key, tiering::DiskSegment segment) {
    if (auto pv = Find(key); pv) {
      RecordAdded(*pv, segment.length, GetDbTableStats(key.first));

      pv->SetIoPending(false);
      pv->SetExternal(segment.offset, segment.length);

      stats_.total_stashes++;
    }
  }

  // Find bin by id and call SetExternal for all contained entries
  void SetExternal(tiering::SmallBins::BinId id, tiering::DiskSegment segment) {
    for (const auto& [sub_dbid, sub_key, sub_segment] : ts_->bins_->ReportStashed(id, segment))
      SetExternal({sub_dbid, sub_key}, sub_segment);
  }

  bool cache_fetched_ = false;

  struct {
    size_t total_stashes = 0, total_cancels = 0, total_fetches = 0;
    size_t total_defrags = 0;
  } stats_;

  TieredStorage* ts_;
  DbSlice* db_slice_;
};

void TieredStorage::ShardOpManager::Defragment(tiering::DiskSegment segment, string_view page) {
  // Note: Bin could've already been deleted, in that case DeleteBin returns an empty list
  for (auto [dbid, hash, item_segment] : ts_->bins_->DeleteBin(segment, page)) {
    // Search for key with the same hash and value pointing to the same segment.
    // If it still exists, it must correspond to the value stored in this bin
    auto predicate = [item_segment](const PrimeKey& key, const PrimeValue& probe) {
      return probe.IsExternal() && tiering::DiskSegment{probe.GetExternalSlice()} == item_segment;
    };
    auto it = db_slice_->GetDBTable(dbid)->prime.FindFirst(hash, predicate);
    if (!IsValid(it))
      continue;

    stats_.total_defrags++;

    // Cut out relevant part of value and restore it to memory
    string_view value = page.substr(item_segment.offset - segment.offset, item_segment.length);
    Upload(dbid, value, true, item_segment.length, &it->second);
  }
}

bool TieredStorage::ShardOpManager::NotifyFetched(EntryId id, string_view value,
                                                  tiering::DiskSegment segment, bool modified) {
  ++stats_.total_fetches;

  if (id == EntryId{kFragmentedBin}) {  // Generally we read whole bins only for defrag
    Defragment(segment, value);
    return true;  // delete
  }

  // 1. When modified is true we MUST upload the value back to memory.
  // 2. On the other hand, if read is caused by snapshotting we do not want to fetch it.
  //    Currently, our heuristic is not very smart, because we stop uploading any reads during
  //    the snapshotting.
  // TODO: to revisit this when we rewrite it with more efficient snapshotting algorithm.

  bool should_upload = modified || (cache_fetched_ && !SliceSnapshot::IsSnaphotInProgress());

  if (!should_upload)
    return false;

  auto key = get<OpManager::KeyRef>(id);
  auto* pv = Find(key);
  if (pv && pv->IsExternal() && segment == pv->GetExternalSlice()) {
    bool is_raw = !modified;
    Upload(key.first, value, is_raw, segment.length, pv);
    return true;
  }

  LOG(DFATAL) << "Internal error, should not reach this";
  return false;
}

bool TieredStorage::ShardOpManager::NotifyDelete(tiering::DiskSegment segment) {
  if (OccupiesWholePages(segment.length))
    return true;

  auto bin = ts_->bins_->Delete(segment);
  if (bin.empty) {
    return true;
  }

  if (bin.fragmented) {
    // Trigger read to signal need for defragmentation. NotifyFetched will handle it.
    VLOG(1) << "Enqueueing bin defragmentation for: x" << bin.segment.offset;
    auto cb = [dummy = 5](bool, std::string*) -> bool {
      (void)dummy;  // a hack to make cb non constexpr that confuses some old) compilers.
      return false;
    };
    Enqueue(kFragmentedBin, bin.segment, std::move(cb));
  }

  return false;
}

TieredStorage::TieredStorage(DbSlice* db_slice, size_t max_size)
    : op_manager_{make_unique<ShardOpManager>(this, db_slice, max_size)},
      bins_{make_unique<tiering::SmallBins>()} {
  write_depth_limit_ = absl::GetFlag(FLAGS_tiered_storage_write_depth);
}

TieredStorage::~TieredStorage() {
}

error_code TieredStorage::Open(string_view path) {
  return op_manager_->Open(absl::StrCat(path, ProactorBase::me()->GetPoolIndex()));
}

void TieredStorage::Close() {
  op_manager_->Close();
}

util::fb2::Future<string> TieredStorage::Read(DbIndex dbid, string_view key,
                                              const PrimeValue& value) {
  DCHECK(value.IsExternal());
  util::fb2::Future<string> future;

  // The raw_val passed to cb might need decoding based on the encoding mask of the "value" object.
  // We save the mask in decoder and use it to decode the final string that Read should resolve.
  PrimeValue decoder;
  decoder.ImportExternal(value);

  auto cb = [future, decoder = std::move(decoder)](bool is_raw, const string* raw_val) mutable {
    future.Resolve(DecodeString(is_raw, *raw_val, std::move(decoder)));
    return false;  // was not modified
  };

  op_manager_->Enqueue(KeyRef(dbid, key), value.GetExternalSlice(), std::move(cb));
  return future;
}

void TieredStorage::Read(DbIndex dbid, std::string_view key, const PrimeValue& value,
                         std::function<void(const std::string&)> readf) {
  DCHECK(value.IsExternal());

  PrimeValue decoder;
  decoder.ImportExternal(value);

  auto cb = [readf = std::move(readf), decoder = std::move(decoder)](
                bool is_raw, const string* raw_val) mutable {
    readf(DecodeString(is_raw, *raw_val, std::move(decoder)));
    return false;
  };
  op_manager_->Enqueue(KeyRef(dbid, key), value.GetExternalSlice(), std::move(cb));
}

template <typename T>
util::fb2::Future<T> TieredStorage::Modify(DbIndex dbid, std::string_view key,
                                           const PrimeValue& value,
                                           std::function<T(std::string*)> modf) {
  DCHECK(value.IsExternal());
  util::fb2::Future<T> future;
  PrimeValue decoder;
  decoder.ImportExternal(value);

  auto cb = [future, modf = std::move(modf), decoder = std::move(decoder)](
                bool is_raw, std::string* raw_val) mutable {
    if (is_raw) {
      decoder.Materialize(*raw_val, true);
      decoder.GetString(raw_val);
    }
    future.Resolve(modf(raw_val));
    return true;
  };
  op_manager_->Enqueue(KeyRef(dbid, key), value.GetExternalSlice(), std::move(cb));
  return future;
}

// Instantiate for size_t only - used in string_family's OpExtend.
template util::fb2::Future<size_t> TieredStorage::Modify(DbIndex dbid, std::string_view key,
                                                         const PrimeValue& value,
                                                         std::function<size_t(std::string*)> modf);

bool TieredStorage::TryStash(DbIndex dbid, string_view key, PrimeValue* value) {
  if (!ShouldStash(*value))
    return false;

  // This invariant should always hold because ShouldStash tests for IoPending flag.
  DCHECK(!bins_->IsPending(dbid, key));

  // TODO: When we are low on memory we should introduce a back-pressure, to avoid OOMs
  // with a lot of underutilized disk space.
  if (op_manager_->GetStats().pending_stash_cnt >= write_depth_limit_) {
    ++stats_.stash_overflow_cnt;
    return false;
  }

  StringOrView raw_string = value->GetRawString();
  value->SetIoPending(true);

  tiering::OpManager::EntryId id;
  error_code ec;
  if (OccupiesWholePages(value->Size())) {  // large enough for own page
    id = KeyRef(dbid, key);
    ec = op_manager_->Stash(id, raw_string.view(), {});
  } else if (auto bin = bins_->Stash(dbid, key, raw_string.view(), {}); bin) {
    id = bin->first;
    ec = op_manager_->Stash(id, bin->second, {});
  }

  if (ec) {
    LOG(ERROR) << "Stash failed immediately" << ec.message();
    visit([this](auto id) { op_manager_->ClearIoPending(id); }, id);
    return false;
  }

  return true;
}

void TieredStorage::Delete(DbIndex dbid, PrimeValue* value) {
  DCHECK(value->IsExternal());
  ++stats_.total_deletes;

  tiering::DiskSegment segment = value->GetExternalSlice();
  op_manager_->DeleteOffloaded(segment);
  value->Reset();
  RecordDeleted(*value, segment.length, op_manager_->GetDbTableStats(dbid));
}

void TieredStorage::CancelStash(DbIndex dbid, std::string_view key, PrimeValue* value) {
  DCHECK(value->HasIoPending());
  if (OccupiesWholePages(value->Size())) {
    op_manager_->Delete(KeyRef(dbid, key));
  } else if (auto bin = bins_->Delete(dbid, key); bin) {
    op_manager_->Delete(*bin);
  }
  value->SetIoPending(false);
}

float TieredStorage::WriteDepthUsage() const {
  return 1.0f * op_manager_->GetStats().pending_stash_cnt / write_depth_limit_;
}

TieredStats TieredStorage::GetStats() const {
  TieredStats stats{};

  {  // ShardOpManager stats
    auto shard_stats = op_manager_->stats_;
    stats.total_fetches = shard_stats.total_fetches;
    stats.total_stashes = shard_stats.total_stashes;
    stats.total_cancels = shard_stats.total_cancels;
    stats.total_defrags = shard_stats.total_defrags;
  }

  {  // OpManager stats
    tiering::OpManager::Stats op_stats = op_manager_->GetStats();
    stats.pending_read_cnt = op_stats.pending_read_cnt;
    stats.pending_stash_cnt = op_stats.pending_stash_cnt;
    stats.allocated_bytes = op_stats.disk_stats.allocated_bytes;
    stats.capacity_bytes = op_stats.disk_stats.capacity_bytes;
    stats.total_heap_buf_allocs = op_stats.disk_stats.heap_buf_alloc_count;
    stats.total_registered_buf_allocs = op_stats.disk_stats.registered_buf_alloc_count;
  }

  {  // SmallBins stats
    tiering::SmallBins::Stats bins_stats = bins_->GetStats();
    stats.small_bins_cnt = bins_stats.stashed_bins_cnt;
    stats.small_bins_entries_cnt = bins_stats.stashed_entries_cnt;
    stats.small_bins_filling_bytes = bins_stats.current_bin_bytes;
  }

  {  // Own stats
    stats.total_stash_overflows = stats_.stash_overflow_cnt;
  }
  return stats;
}

void TieredStorage::RunOffloading(DbIndex dbid) {
  const size_t kMaxIterations = 500;

  if (SliceSnapshot::IsSnaphotInProgress())
    return;

  // Don't run offloading if there's only very little space left
  auto disk_stats = op_manager_->GetStats().disk_stats;
  if (disk_stats.allocated_bytes + kMaxIterations / 2 * tiering::kPageSize >
      disk_stats.max_file_size)
    return;

  auto cb = [this, dbid, tmp = std::string{}](PrimeIterator it) mutable {
    TryStash(dbid, it->first.GetSlice(&tmp), &it->second);
  };

  PrimeTable& table = op_manager_->db_slice_->GetDBTable(dbid)->prime;
  PrimeTable::Cursor start_cursor{};

  // Loop while we haven't traversed all entries or reached our stash io device limit.
  // Keep number of iterations below resonable limit to keep datastore always responsive
  size_t iterations = 0;
  do {
    if (op_manager_->GetStats().pending_stash_cnt >= write_depth_limit_)
      break;
    offloading_cursor_ = table.TraverseBySegmentOrder(offloading_cursor_, cb);
  } while (offloading_cursor_ != start_cursor && iterations++ < kMaxIterations);
}

bool TieredStorage::ShouldStash(const PrimeValue& pv) const {
  auto disk_stats = op_manager_->GetStats().disk_stats;
  return !pv.IsExternal() && !pv.HasIoPending() && pv.ObjType() == OBJ_STRING &&
         pv.Size() >= kMinValueSize &&
         disk_stats.allocated_bytes + tiering::kPageSize + pv.Size() < disk_stats.max_file_size;
}

}  // namespace dfly
