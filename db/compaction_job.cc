//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction_job.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <list>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/event_helpers.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/merge_helper.h"
#include "db/memtable_list.h"
#include "db/merge_context.h"
#include "db/version_set.h"
#include "port/port.h"
#include "port/likely.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "table/block.h"
#include "table/block_based_table_factory.h"
#include "table/merger.h"
#include "table/table_builder.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/log_buffer.h"
#include "util/mutexlock.h"
#include "util/perf_context_imp.h"
#include "util/iostats_context_imp.h"
#include "util/stop_watch.h"
#include "util/string_util.h"
#include "util/sync_point.h"
#include "util/thread_status_util.h"

namespace rocksdb {

struct CompactionJob::CompactionState {
  Compaction* const compaction;

  // Files produced by compaction
  struct Output {
    uint64_t number;
    uint32_t path_id;
    uint64_t file_size;
    InternalKey smallest, largest;
    SequenceNumber smallest_seqno, largest_seqno;
    bool need_compaction;
  };
  std::vector<Output> outputs;

  // State kept for output being generated
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;

  uint64_t total_bytes;

  Output* current_output() { return &outputs[outputs.size() - 1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        total_bytes(0),
        num_input_records(0),
        num_output_records(0) {}

  std::vector<std::string> key_str_buf_;
  std::vector<std::string> existing_value_str_buf_;
  // new_value_buf_ will only be appended if a value changes
  std::vector<std::string> new_value_buf_;
  // if values_changed_buf_[i] is true
  // new_value_buf_ will add a new entry with the changed value
  std::vector<bool> value_changed_buf_;
  // to_delete_buf_[i] is true iff key_buf_[i] is deleted
  std::vector<bool> to_delete_buf_;

  std::vector<std::string> other_key_str_buf_;
  std::vector<std::string> other_value_str_buf_;

  std::vector<Slice> combined_key_buf_;
  std::vector<Slice> combined_value_buf_;

  std::string cur_prefix_;

  uint64_t num_input_records;
  uint64_t num_output_records;

  // Buffers the kv-pair that will be run through compaction filter V2
  // in the future.
  void BufferKeyValueSlices(const Slice& key, const Slice& value) {
    key_str_buf_.emplace_back(key.ToString());
    existing_value_str_buf_.emplace_back(value.ToString());
  }

  // Buffers the kv-pair that will not be run through compaction filter V2
  // in the future.
  void BufferOtherKeyValueSlices(const Slice& key, const Slice& value) {
    other_key_str_buf_.emplace_back(key.ToString());
    other_value_str_buf_.emplace_back(value.ToString());
  }

  // Add a kv-pair to the combined buffer
  void AddToCombinedKeyValueSlices(const Slice& key, const Slice& value) {
    // The real strings are stored in the batch buffers
    combined_key_buf_.emplace_back(key);
    combined_value_buf_.emplace_back(value);
  }

  // Merging the two buffers
  void MergeKeyValueSliceBuffer(const InternalKeyComparator* comparator) {
    size_t i = 0;
    size_t j = 0;
    size_t total_size = key_str_buf_.size() + other_key_str_buf_.size();
    combined_key_buf_.reserve(total_size);
    combined_value_buf_.reserve(total_size);

    while (i + j < total_size) {
      int comp_res = 0;
      if (i < key_str_buf_.size() && j < other_key_str_buf_.size()) {
        comp_res = comparator->Compare(key_str_buf_[i], other_key_str_buf_[j]);
      } else if (i >= key_str_buf_.size() && j < other_key_str_buf_.size()) {
        comp_res = 1;
      } else if (j >= other_key_str_buf_.size() && i < key_str_buf_.size()) {
        comp_res = -1;
      }
      if (comp_res > 0) {
        AddToCombinedKeyValueSlices(other_key_str_buf_[j],
                                    other_value_str_buf_[j]);
        j++;
      } else if (comp_res < 0) {
        AddToCombinedKeyValueSlices(key_str_buf_[i],
                                    existing_value_str_buf_[i]);
        i++;
      }
    }
  }

  void CleanupBatchBuffer() {
    to_delete_buf_.clear();
    key_str_buf_.clear();
    existing_value_str_buf_.clear();
    new_value_buf_.clear();
    value_changed_buf_.clear();

    to_delete_buf_.shrink_to_fit();
    key_str_buf_.shrink_to_fit();
    existing_value_str_buf_.shrink_to_fit();
    new_value_buf_.shrink_to_fit();
    value_changed_buf_.shrink_to_fit();

    other_key_str_buf_.clear();
    other_value_str_buf_.clear();
    other_key_str_buf_.shrink_to_fit();
    other_value_str_buf_.shrink_to_fit();
  }

  void CleanupMergedBuffer() {
    combined_key_buf_.clear();
    combined_value_buf_.clear();
    combined_key_buf_.shrink_to_fit();
    combined_value_buf_.shrink_to_fit();
  }
};

CompactionJob::CompactionJob(
    int job_id, Compaction* compaction, const DBOptions& db_options,
    const EnvOptions& env_options, VersionSet* versions,
    std::atomic<bool>* shutting_down, LogBuffer* log_buffer,
    Directory* db_directory, Directory* output_directory, Statistics* stats,
    std::vector<SequenceNumber> existing_snapshots,
    std::shared_ptr<Cache> table_cache,
    std::function<uint64_t()> yield_callback, EventLogger* event_logger,
    bool paranoid_file_checks, const std::string& dbname,
    CompactionJobStats* compaction_job_stats)
    : job_id_(job_id),
      compact_(new CompactionState(compaction)),
      compaction_job_stats_(compaction_job_stats),
      compaction_stats_(1),
      dbname_(dbname),
      db_options_(db_options),
      env_options_(env_options),
      env_(db_options.env),
      versions_(versions),
      shutting_down_(shutting_down),
      log_buffer_(log_buffer),
      db_directory_(db_directory),
      output_directory_(output_directory),
      stats_(stats),
      existing_snapshots_(std::move(existing_snapshots)),
      table_cache_(std::move(table_cache)),
      yield_callback_(std::move(yield_callback)),
      event_logger_(event_logger),
      paranoid_file_checks_(paranoid_file_checks) {
  assert(log_buffer_ != nullptr);
  ThreadStatusUtil::SetColumnFamily(compact_->compaction->column_family_data());
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_COMPACTION);
  ReportStartedCompaction(compaction);
}

CompactionJob::~CompactionJob() {
  assert(compact_ == nullptr);
  ThreadStatusUtil::ResetThreadStatus();
}

void CompactionJob::ReportStartedCompaction(
    Compaction* compaction) {
  ThreadStatusUtil::SetColumnFamily(
      compact_->compaction->column_family_data());

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_JOB_ID,
      job_id_);

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_INPUT_OUTPUT_LEVEL,
      (static_cast<uint64_t>(compact_->compaction->start_level()) << 32) +
          compact_->compaction->output_level());

  // In the current design, a CompactionJob is always created
  // for non-trivial compaction.
  assert(compaction->IsTrivialMove() == false ||
         compaction->is_manual_compaction() == true);

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_PROP_FLAGS,
      compaction->is_manual_compaction() +
          (compaction->deletion_compaction() << 1));

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_TOTAL_INPUT_BYTES,
      compaction->CalculateTotalInputSize());

  IOSTATS_RESET(bytes_written);
  IOSTATS_RESET(bytes_read);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_WRITTEN, 0);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_READ, 0);

  // Set the thread operation after operation properties
  // to ensure GetThreadList() can always show them all together.
  ThreadStatusUtil::SetThreadOperation(
      ThreadStatus::OP_COMPACTION);

  if (compaction_job_stats_) {
    compaction_job_stats_->is_manual_compaction =
        compaction->is_manual_compaction();
  }
}

void CompactionJob::Prepare() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_PREPARE);
  compact_->CleanupBatchBuffer();
  compact_->CleanupMergedBuffer();

  // Generate file_levels_ for compaction berfore making Iterator
  ColumnFamilyData* cfd __attribute__((unused)) =
      compact_->compaction->column_family_data();
  assert(cfd != nullptr);

  assert(cfd->current()->storage_info()->NumLevelFiles(
             compact_->compaction->level()) > 0);
  assert(compact_->builder == nullptr);
  assert(!compact_->outfile);

  visible_at_tip_ = 0;
  latest_snapshot_ = 0;
  if (existing_snapshots_.size() == 0) {
    // optimize for fast path if there are no snapshots
    visible_at_tip_ = versions_->LastSequence();
    earliest_snapshot_ = visible_at_tip_;
  } else {
    latest_snapshot_ = existing_snapshots_.back();
    // Add the current seqno as the 'latest' virtual
    // snapshot to the end of this list.
    existing_snapshots_.push_back(versions_->LastSequence());
    earliest_snapshot_ = existing_snapshots_[0];
  }

  // Is this compaction producing files at the bottommost level?
  bottommost_level_ = compact_->compaction->bottommost_level();
}

Status CompactionJob::Run() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_RUN);
  TEST_SYNC_POINT("CompactionJob::Run():Start");
  log_buffer_->FlushBufferToLog();
  ColumnFamilyData* cfd = compact_->compaction->column_family_data();
  auto* compaction = compact_->compaction;
  LogCompaction(cfd, compaction);

  const uint64_t start_micros = env_->NowMicros();
  std::unique_ptr<Iterator> input(
      versions_->MakeInputIterator(compact_->compaction));
  input->SeekToFirst();

  std::unique_ptr<CompactionFilterV2> compaction_filter_from_factory_v2 =
      compact_->compaction->CreateCompactionFilterV2();
  auto compaction_filter_v2 = compaction_filter_from_factory_v2.get();

  Status status;
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions
  if (!compaction_filter_v2) {
    status = ProcessKeyValueCompaction(&imm_micros, input.get(), false);
  } else {
    status = ProcessPrefixBatches(cfd, &imm_micros, input.get(),
        compaction_filter_v2);
  }

  if (status.ok() &&
      (shutting_down_->load(std::memory_order_acquire) || cfd->IsDropped())) {
    status = Status::ShutdownInProgress(
        "Database shutdown or Column family drop during compaction");
  }
  if (status.ok() && compact_->builder != nullptr) {
    status = FinishCompactionOutputFile(input->status());
  }
  if (status.ok()) {
    status = input->status();
  }
  input.reset();

  if (output_directory_ && !db_options_.disableDataSync) {
    output_directory_->Fsync();
  }

  compaction_stats_.micros = env_->NowMicros() - start_micros - imm_micros;
  MeasureTime(stats_, COMPACTION_TIME, compaction_stats_.micros);
  UpdateCompactionStats();

  RecordCompactionIOStats();

  LogFlush(db_options_.info_log);
  TEST_SYNC_POINT("CompactionJob::Run():End");
  return status;
}

void CompactionJob::Install(Status* status,
                            const MutableCFOptions& mutable_cf_options,
                            InstrumentedMutex* db_mutex) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_INSTALL);
  db_mutex->AssertHeld();
  ColumnFamilyData* cfd = compact_->compaction->column_family_data();
  cfd->internal_stats()->AddCompactionStats(
      compact_->compaction->output_level(), compaction_stats_);

  if (status->ok()) {
    *status = InstallCompactionResults(db_mutex, mutable_cf_options);
  }
  VersionStorageInfo::LevelSummaryStorage tmp;
  auto vstorage = cfd->current()->storage_info();
  const auto& stats = compaction_stats_;
  LogToBuffer(
      log_buffer_,
      "[%s] compacted to: %s, MB/sec: %.1f rd, %.1f wr, level %d, "
      "files in(%d, %d) out(%d) "
      "MB in(%.1f, %.1f) out(%.1f), read-write-amplify(%.1f) "
      "write-amplify(%.1f) %s, records in: %d, records dropped: %d\n",
      cfd->GetName().c_str(), vstorage->LevelSummary(&tmp),
      (stats.bytes_read_non_output_levels + stats.bytes_read_output_level) /
          static_cast<double>(stats.micros),
      stats.bytes_written / static_cast<double>(stats.micros),
      compact_->compaction->output_level(),
      stats.num_input_files_in_non_output_levels,
      stats.num_input_files_in_output_level,
      stats.num_output_files,
      stats.bytes_read_non_output_levels / 1048576.0,
      stats.bytes_read_output_level / 1048576.0,
      stats.bytes_written / 1048576.0,
      (stats.bytes_written + stats.bytes_read_output_level +
       stats.bytes_read_non_output_levels) /
          static_cast<double>(stats.bytes_read_non_output_levels),
      stats.bytes_written /
          static_cast<double>(stats.bytes_read_non_output_levels),
      status->ToString().c_str(), stats.num_input_records,
      stats.num_dropped_records);

  UpdateCompactionJobStats(stats);

  auto stream = event_logger_->LogToBuffer(log_buffer_);
  stream << "job" << job_id_ << "event"
         << "compaction_finished"
         << "output_level" << compact_->compaction->output_level()
         << "num_output_files" << compact_->outputs.size()
         << "total_output_size" << compact_->total_bytes
         << "num_input_records" << compact_->num_input_records
         << "num_output_records" << compact_->num_output_records;
  stream << "lsm_state";
  stream.StartArray();
  for (int level = 0; level < vstorage->num_levels(); ++level) {
    stream << vstorage->NumLevelFiles(level);
  }
  stream.EndArray();

  CleanupCompaction(*status);
}

Status CompactionJob::ProcessPrefixBatches(
    ColumnFamilyData* cfd,
    int64_t* imm_micros,
    Iterator* input,
    CompactionFilterV2* compaction_filter_v2) {
  // temp_backup_input always point to the start of the current buffer
  // temp_backup_input = backup_input;
  // iterate through input,
  // 1) buffer ineligible keys and value keys into 2 separate buffers;
  // 2) send value_buffer to compaction filter and alternate the values;
  // 3) merge value_buffer with ineligible_value_buffer;
  // 4) run the modified "compaction" using the old for loop.
  ParsedInternalKey ikey;
  Status status;
  bool prefix_initialized = false;
  shared_ptr<Iterator> backup_input(
      versions_->MakeInputIterator(compact_->compaction));
  backup_input->SeekToFirst();
  uint64_t total_filter_time = 0;
  while (backup_input->Valid() &&
         !shutting_down_->load(std::memory_order_acquire) &&
         !cfd->IsDropped()) {
    // FLUSH preempts compaction
    // TODO(icanadi) this currently only checks if flush is necessary on
    // compacting column family. we should also check if flush is necessary on
    // other column families, too

    imm_micros += yield_callback_();

    Slice key = backup_input->key();
    Slice value = backup_input->value();

    if (!ParseInternalKey(key, &ikey)) {
      // log error
      Log(InfoLogLevel::WARN_LEVEL, db_options_.info_log,
          "[%s] [JOB %d] Failed to parse key: %s", cfd->GetName().c_str(),
          job_id_, key.ToString().c_str());
      continue;
    } else {
      const SliceTransform* transformer =
          cfd->ioptions()->compaction_filter_factory_v2->GetPrefixExtractor();
      const auto key_prefix = transformer->Transform(ikey.user_key);
      if (!prefix_initialized) {
        compact_->cur_prefix_ = key_prefix.ToString();
        prefix_initialized = true;
      }
      // If the prefix remains the same, keep buffering
      if (key_prefix.compare(Slice(compact_->cur_prefix_)) == 0) {
        // Apply the compaction filter V2 to all the kv pairs sharing
        // the same prefix
        if (ikey.type == kTypeValue &&
            (visible_at_tip_ || ikey.sequence > latest_snapshot_)) {
          // Buffer all keys sharing the same prefix for CompactionFilterV2
          // Iterate through keys to check prefix
          compact_->BufferKeyValueSlices(key, value);
        } else {
          // buffer ineligible keys
          compact_->BufferOtherKeyValueSlices(key, value);
        }
        backup_input->Next();
        continue;
        // finish changing values for eligible keys
      } else {
        // Now prefix changes, this batch is done.
        // Call compaction filter on the buffered values to change the value
        if (compact_->key_str_buf_.size() > 0) {
          uint64_t time = 0;
          CallCompactionFilterV2(compaction_filter_v2, &time);
          total_filter_time += time;
        }
        compact_->cur_prefix_ = key_prefix.ToString();
      }
    }

    // Merge this batch of data (values + ineligible keys)
    compact_->MergeKeyValueSliceBuffer(&cfd->internal_comparator());

    // Done buffering for the current prefix. Spit it out to disk
    // Now just iterate through all the kv-pairs
    status = ProcessKeyValueCompaction(imm_micros, input, true);

    if (!status.ok()) {
      break;
    }

    // After writing the kv-pairs, we can safely remove the reference
    // to the string buffer and clean them up
    compact_->CleanupBatchBuffer();
    compact_->CleanupMergedBuffer();
    // Buffer the key that triggers the mismatch in prefix
    if (ikey.type == kTypeValue &&
        (visible_at_tip_ || ikey.sequence > latest_snapshot_)) {
      compact_->BufferKeyValueSlices(key, value);
    } else {
      compact_->BufferOtherKeyValueSlices(key, value);
    }
    backup_input->Next();
    if (!backup_input->Valid()) {
      // If this is the single last value, we need to merge it.
      if (compact_->key_str_buf_.size() > 0) {
        uint64_t time = 0;
        CallCompactionFilterV2(compaction_filter_v2, &time);
        total_filter_time += time;
      }
      compact_->MergeKeyValueSliceBuffer(&cfd->internal_comparator());

      status = ProcessKeyValueCompaction(imm_micros, input, true);
      if (!status.ok()) {
        break;
      }

      compact_->CleanupBatchBuffer();
      compact_->CleanupMergedBuffer();
    }
  }  // done processing all prefix batches
  // finish the last batch
  if (status.ok()) {
    if (compact_->key_str_buf_.size() > 0) {
      uint64_t time = 0;
      CallCompactionFilterV2(compaction_filter_v2, &time);
      total_filter_time += time;
    }
    compact_->MergeKeyValueSliceBuffer(&cfd->internal_comparator());
    status = ProcessKeyValueCompaction(imm_micros, input, true);
  }
  RecordTick(stats_, FILTER_OPERATION_TOTAL_TIME, total_filter_time);
  return status;
}

Status CompactionJob::ProcessKeyValueCompaction(int64_t* imm_micros,
                                                Iterator* input,
                                                bool is_compaction_v2) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_PROCESS_KV);
  size_t combined_idx = 0;
  Status status;
  std::string compaction_filter_value;
  ParsedInternalKey ikey;
  IterKey current_user_key;
  bool has_current_user_key = false;
  IterKey delete_key;
  SequenceNumber last_sequence_for_key __attribute__((unused)) =
      kMaxSequenceNumber;
  SequenceNumber visible_in_snapshot = kMaxSequenceNumber;
  ColumnFamilyData* cfd = compact_->compaction->column_family_data();
  MergeHelper merge(cfd->user_comparator(), cfd->ioptions()->merge_operator,
                    db_options_.info_log.get(),
                    cfd->ioptions()->min_partial_merge_operands,
                    false /* internal key corruption is expected */);
  auto compaction_filter = cfd->ioptions()->compaction_filter;
  std::unique_ptr<CompactionFilter> compaction_filter_from_factory = nullptr;
  if (!compaction_filter) {
    compaction_filter_from_factory =
        compact_->compaction->CreateCompactionFilter();
    compaction_filter = compaction_filter_from_factory.get();
  }

  TEST_SYNC_POINT("CompactionJob::Run():Inprogress");

  int64_t key_drop_user = 0;
  int64_t key_drop_newer_entry = 0;
  int64_t key_drop_obsolete = 0;
  int64_t loop_cnt = 0;

  StopWatchNano timer(env_, stats_ != nullptr);
  uint64_t total_filter_time = 0;
  while (input->Valid() && !shutting_down_->load(std::memory_order_acquire) &&
         !cfd->IsDropped() && status.ok()) {
    compact_->num_input_records++;
    if (++loop_cnt > 1000) {
      RecordDroppedKeys(
          &key_drop_user, &key_drop_newer_entry, &key_drop_obsolete);
      RecordCompactionIOStats();
      loop_cnt = 0;
    }
    // FLUSH preempts compaction
    // TODO(icanadi) this currently only checks if flush is necessary on
    // compacting column family. we should also check if flush is necessary
    // on other column families, too
    (*imm_micros) += yield_callback_();

    Slice key;
    Slice value;
    // If is_compaction_v2 is on, kv-pairs are reset to the prefix batch.
    // This prefix batch should contain results after calling
    // compaction_filter_v2.
    //
    // If is_compaction_v2 is off, this function will go through all the
    // kv-pairs in input.
    if (!is_compaction_v2) {
      key = input->key();
      value = input->value();
    } else {
      if (combined_idx >= compact_->combined_key_buf_.size()) {
        break;
      }
      key = compact_->combined_key_buf_[combined_idx];
      value = compact_->combined_value_buf_[combined_idx];

      ++combined_idx;
    }

    if (compaction_job_stats_ != nullptr) {
      compaction_job_stats_->total_input_raw_key_bytes +=
          input->key().size();
      compaction_job_stats_->total_input_raw_value_bytes +=
          input->value().size();
    }

    if (compact_->compaction->ShouldStopBefore(key) &&
        compact_->builder != nullptr) {
      status = FinishCompactionOutputFile(input->status());
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    bool drop = false;
    bool current_entry_is_merging = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      // TODO: error key stays in db forever? Figure out the intention/rationale
      // v10 error v8 : we cannot hide v8 even though it's pretty obvious.
      current_user_key.Clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
      visible_in_snapshot = kMaxSequenceNumber;
    } else {
      if (compaction_job_stats_ != nullptr && ikey.type == kTypeDeletion) {
        compaction_job_stats_->num_input_deletion_records++;
      }

      if (!has_current_user_key ||
          cfd->user_comparator()->Compare(ikey.user_key,
                                          current_user_key.GetKey()) != 0) {
        // First occurrence of this user key
        current_user_key.SetKey(ikey.user_key);
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
        visible_in_snapshot = kMaxSequenceNumber;
        // apply the compaction filter to the first occurrence of the user key
        if (compaction_filter && !is_compaction_v2 && ikey.type == kTypeValue &&
            (visible_at_tip_ || ikey.sequence > latest_snapshot_)) {
          // If the user has specified a compaction filter and the sequence
          // number is greater than any external snapshot, then invoke the
          // filter.
          // If the return value of the compaction filter is true, replace
          // the entry with a delete marker.
          bool value_changed = false;
          compaction_filter_value.clear();
          if (stats_ != nullptr) {
            timer.Start();
          }
          bool to_delete = compaction_filter->Filter(
              compact_->compaction->level(), ikey.user_key, value,
              &compaction_filter_value, &value_changed);
          total_filter_time += timer.ElapsedNanos();
          if (to_delete) {
            // make a copy of the original key and convert it to a delete
            delete_key.SetInternalKey(ExtractUserKey(key), ikey.sequence,
                                      kTypeDeletion);
            // anchor the key again
            key = delete_key.GetKey();
            // needed because ikey is backed by key
            ParseInternalKey(key, &ikey);
            // no value associated with delete
            value.clear();
            ++key_drop_user;
          } else if (value_changed) {
            value = compaction_filter_value;
          }
        }
      }

      // If there are no snapshots, then this kv affect visibility at tip.
      // Otherwise, search though all existing snapshots to find
      // the earlist snapshot that is affected by this kv.
      SequenceNumber prev_snapshot = 0;  // 0 means no previous snapshot
      SequenceNumber visible =
          visible_at_tip_
              ? visible_at_tip_
              : findEarliestVisibleSnapshot(ikey.sequence, existing_snapshots_,
                                            &prev_snapshot);

      if (visible_in_snapshot == visible) {
        // If the earliest snapshot is which this key is visible in
        // is the same as the visibily of a previous instance of the
        // same key, then this kv is not visible in any snapshot.
        // Hidden by an newer entry for same user key
        // TODO: why not > ?
        assert(last_sequence_for_key >= ikey.sequence);
        drop = true;  // (A)
        ++key_drop_newer_entry;
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= earliest_snapshot_ &&
                 compact_->compaction->KeyNotExistsBeyondOutputLevel(
                     ikey.user_key)) {
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        drop = true;
        ++key_drop_obsolete;
      } else if (ikey.type == kTypeMerge) {
        if (!merge.HasOperator()) {
          LogToBuffer(log_buffer_, "Options::merge_operator is null.");
          status = Status::InvalidArgument(
              "merge_operator is not properly initialized.");
          break;
        }
        // We know the merge type entry is not hidden, otherwise we would
        // have hit (A)
        // We encapsulate the merge related state machine in a different
        // object to minimize change to the existing flow. Turn out this
        // logic could also be nicely re-used for memtable flush purge
        // optimization in BuildTable.
        int steps = 0;
        merge.MergeUntil(input, prev_snapshot, bottommost_level_,
                         db_options_.statistics.get(), &steps, env_);
        // Skip the Merge ops
        combined_idx = combined_idx - 1 + steps;

        current_entry_is_merging = true;
        if (merge.IsSuccess()) {
          // Successfully found Put/Delete/(end-of-key-range) while merging
          // Get the merge result
          key = merge.key();
          ParseInternalKey(key, &ikey);
          value = merge.value();
        } else {
          // Did not find a Put/Delete/(end-of-key-range) while merging
          // We now have some stack of merge operands to write out.
          // NOTE: key,value, and ikey are now referring to old entries.
          //       These will be correctly set below.
          assert(!merge.keys().empty());
          assert(merge.keys().size() == merge.values().size());

          // Hack to make sure last_sequence_for_key is correct
          ParseInternalKey(merge.keys().front(), &ikey);
        }
      }

      last_sequence_for_key = ikey.sequence;
      visible_in_snapshot = visible;
    }

    if (!drop) {
      // We may write a single key (e.g.: for Put/Delete or successful merge).
      // Or we may instead have to write a sequence/list of keys.
      // We have to write a sequence iff we have an unsuccessful merge
      if (current_entry_is_merging && !merge.IsSuccess()) {
        const auto& keys = merge.keys();
        const auto& values = merge.values();
        std::deque<std::string>::const_reverse_iterator key_iter =
          keys.rbegin();  // The back (*rbegin()) is the first key
        std::deque<std::string>::const_reverse_iterator value_iter =
          values.rbegin();

        key = Slice(*key_iter);
        value = Slice(*value_iter);

        // We have a list of keys to write, traverse the list.
        while (true) {
          status = WriteKeyValue(key, value, ikey, input->status());
          if (!status.ok()) {
            break;
          }

          ++key_iter;
          ++value_iter;

          // If at end of list
          if (key_iter == keys.rend() || value_iter == values.rend()) {
            // Sanity Check: if one ends, then both end
            assert(key_iter == keys.rend() && value_iter == values.rend());
            break;
          }

          // Otherwise not at end of list. Update key, value, and ikey.
          key = Slice(*key_iter);
          value = Slice(*value_iter);
          ParseInternalKey(key, &ikey);
        }
      } else {
        // There is only one item to be written out
        status = WriteKeyValue(key, value, ikey, input->status());
      }
    }    // if (!drop)

    // MergeUntil has moved input to the next entry
    if (!current_entry_is_merging) {
      input->Next();
    }
  }
  RecordTick(stats_, FILTER_OPERATION_TOTAL_TIME, total_filter_time);
  RecordDroppedKeys(&key_drop_user, &key_drop_newer_entry, &key_drop_obsolete);
  RecordCompactionIOStats();

  return status;
}

Status CompactionJob::WriteKeyValue(const Slice& key, const Slice& value,
    const ParsedInternalKey& ikey, const Status& input_status) {
  Slice newkey(key.data(), key.size());
  std::string kstr;

  // Zeroing out the sequence number leads to better compression.
  // If this is the bottommost level (no files in lower levels)
  // and the earliest snapshot is larger than this seqno
  // then we can squash the seqno to zero.
  if (bottommost_level_ && ikey.sequence < earliest_snapshot_ &&
      ikey.type != kTypeMerge) {
    assert(ikey.type != kTypeDeletion);
    // make a copy because updating in place would cause problems
    // with the priority queue that is managing the input key iterator
    kstr.assign(key.data(), key.size());
    UpdateInternalKey(&kstr, (uint64_t)0, ikey.type);
    newkey = Slice(kstr);
  }

  // Open output file if necessary
  if (compact_->builder == nullptr) {
    Status status = OpenCompactionOutputFile();
    if (!status.ok()) {
      return status;
    }
  }

  SequenceNumber seqno = GetInternalKeySeqno(newkey);
  if (compact_->builder->NumEntries() == 0) {
    compact_->current_output()->smallest.DecodeFrom(newkey);
    compact_->current_output()->smallest_seqno = seqno;
  } else {
    compact_->current_output()->smallest_seqno =
        std::min(compact_->current_output()->smallest_seqno, seqno);
  }
  compact_->current_output()->largest.DecodeFrom(newkey);
  compact_->builder->Add(newkey, value);
  compact_->num_output_records++;
  compact_->current_output()->largest_seqno =
    std::max(compact_->current_output()->largest_seqno, seqno);

  // Close output file if it is big enough
  Status status;
  if (compact_->builder->FileSize() >=
      compact_->compaction->max_output_file_size()) {
    status = FinishCompactionOutputFile(input_status);
  }

  return status;
}

void CompactionJob::RecordDroppedKeys(
    int64_t* key_drop_user,
    int64_t* key_drop_newer_entry,
    int64_t* key_drop_obsolete) {
  if (*key_drop_user > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_USER, *key_drop_user);
    *key_drop_user = 0;
  }
  if (*key_drop_newer_entry > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_NEWER_ENTRY, *key_drop_newer_entry);
    if (compaction_job_stats_) {
      compaction_job_stats_->num_records_replaced += *key_drop_newer_entry;
    }
    *key_drop_newer_entry = 0;
  }
  if (*key_drop_obsolete > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_OBSOLETE, *key_drop_obsolete);
    if (compaction_job_stats_) {
      compaction_job_stats_->num_expired_deletion_records
          += *key_drop_obsolete;
    }
    *key_drop_obsolete = 0;
  }
}

void CompactionJob::CallCompactionFilterV2(
    CompactionFilterV2* compaction_filter_v2, uint64_t* time) {
  if (compact_ == nullptr || compaction_filter_v2 == nullptr) {
    return;
  }
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_FILTER_V2);

  // Assemble slice vectors for user keys and existing values.
  // We also keep track of our parsed internal key structs because
  // we may need to access the sequence number in the event that
  // keys are garbage collected during the filter process.
  std::vector<ParsedInternalKey> ikey_buf;
  std::vector<Slice> user_key_buf;
  std::vector<Slice> existing_value_buf;

  for (const auto& key : compact_->key_str_buf_) {
    ParsedInternalKey ikey;
    ParseInternalKey(Slice(key), &ikey);
    ikey_buf.emplace_back(ikey);
    user_key_buf.emplace_back(ikey.user_key);
  }
  for (const auto& value : compact_->existing_value_str_buf_) {
    existing_value_buf.emplace_back(Slice(value));
  }

  // If the user has specified a compaction filter and the sequence
  // number is greater than any external snapshot, then invoke the
  // filter.
  // If the return value of the compaction filter is true, replace
  // the entry with a delete marker.
  StopWatchNano timer(env_, stats_ != nullptr);
  compact_->to_delete_buf_ = compaction_filter_v2->Filter(
      compact_->compaction->level(), user_key_buf, existing_value_buf,
      &compact_->new_value_buf_, &compact_->value_changed_buf_);
  *time = timer.ElapsedNanos();
  // new_value_buf_.size() <= to_delete__buf_.size(). "=" iff all
  // kv-pairs in this compaction run needs to be deleted.
  assert(compact_->to_delete_buf_.size() == compact_->key_str_buf_.size());
  assert(compact_->to_delete_buf_.size() ==
         compact_->existing_value_str_buf_.size());
  assert(compact_->value_changed_buf_.empty() ||
         compact_->to_delete_buf_.size() ==
         compact_->value_changed_buf_.size());

  int new_value_idx = 0;
  for (unsigned int i = 0; i < compact_->to_delete_buf_.size(); ++i) {
    if (compact_->to_delete_buf_[i]) {
      // update the string buffer directly
      // the Slice buffer points to the updated buffer
      UpdateInternalKey(&compact_->key_str_buf_[i], ikey_buf[i].sequence,
                        kTypeDeletion);

      // no value associated with delete
      compact_->existing_value_str_buf_[i].clear();
      RecordTick(stats_, COMPACTION_KEY_DROP_USER);
    } else if (!compact_->value_changed_buf_.empty() &&
        compact_->value_changed_buf_[i]) {
      compact_->existing_value_str_buf_[i] =
          compact_->new_value_buf_[new_value_idx++];
    }
  }  // for
}

Status CompactionJob::FinishCompactionOutputFile(const Status& input_status) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_SYNC_FILE);
  assert(compact_ != nullptr);
  assert(compact_->outfile);
  assert(compact_->builder != nullptr);

  const uint64_t output_number = compact_->current_output()->number;
  const uint32_t output_path_id = compact_->current_output()->path_id;
  assert(output_number != 0);

  TableProperties table_properties;
  // Check for iterator errors
  Status s = input_status;
  const uint64_t current_entries = compact_->builder->NumEntries();
  compact_->current_output()->need_compaction =
      compact_->builder->NeedCompact();
  if (s.ok()) {
    s = compact_->builder->Finish();
  } else {
    compact_->builder->Abandon();
  }
  const uint64_t current_bytes = compact_->builder->FileSize();
  compact_->current_output()->file_size = current_bytes;
  compact_->total_bytes += current_bytes;

  // Finish and check for file errors
  if (s.ok() && !db_options_.disableDataSync) {
    if (db_options_.use_fsync) {
      StopWatch sw(env_, stats_, COMPACTION_OUTFILE_SYNC_MICROS);
      s = compact_->outfile->Fsync();
    } else {
      StopWatch sw(env_, stats_, COMPACTION_OUTFILE_SYNC_MICROS);
      s = compact_->outfile->Sync();
    }
  }
  if (s.ok()) {
    s = compact_->outfile->Close();
  }
  compact_->outfile.reset();

  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    ColumnFamilyData* cfd = compact_->compaction->column_family_data();
    FileDescriptor fd(output_number, output_path_id, current_bytes);
    Iterator* iter = cfd->table_cache()->NewIterator(
        ReadOptions(), env_options_, cfd->internal_comparator(), fd);
    s = iter->status();

    if (s.ok() && paranoid_file_checks_) {
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {}
      s = iter->status();
    }

    delete iter;
    if (s.ok()) {
      TableFileCreationInfo info(compact_->builder->GetTableProperties());
      info.db_name = dbname_;
      info.cf_name = cfd->GetName();
      info.file_path = TableFileName(cfd->ioptions()->db_paths,
                                     fd.GetNumber(), fd.GetPathId());
      info.file_size = fd.GetFileSize();
      info.job_id = job_id_;
      Log(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
          "[%s] [JOB %d] Generated table #%" PRIu64 ": %" PRIu64
          " keys, %" PRIu64 " bytes%s",
          cfd->GetName().c_str(), job_id_, output_number, current_entries,
          current_bytes,
          compact_->current_output()->need_compaction ? " (need compaction)"
                                                      : "");
      EventHelpers::LogAndNotifyTableFileCreation(
          event_logger_, cfd->ioptions()->listeners, fd, info);
    }
  }
  compact_->builder.reset();
  return s;
}

Status CompactionJob::InstallCompactionResults(
    InstrumentedMutex* db_mutex, const MutableCFOptions& mutable_cf_options) {
  db_mutex->AssertHeld();

  auto* compaction = compact_->compaction;
  // paranoia: verify that the files that we started with
  // still exist in the current version and in the same original level.
  // This ensures that a concurrent compaction did not erroneously
  // pick the same files to compact_.
  if (!versions_->VerifyCompactionFileConsistency(compaction)) {
    Compaction::InputLevelSummaryBuffer inputs_summary;

    Log(InfoLogLevel::ERROR_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] Compaction %s aborted",
        compaction->column_family_data()->GetName().c_str(), job_id_,
        compaction->InputLevelSummary(&inputs_summary));
    return Status::Corruption("Compaction input files inconsistent");
  }

  {
    Compaction::InputLevelSummaryBuffer inputs_summary;
    Log(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] Compacted %s => %" PRIu64 " bytes",
        compaction->column_family_data()->GetName().c_str(), job_id_,
        compaction->InputLevelSummary(&inputs_summary), compact_->total_bytes);
  }

  // Add compaction outputs
  compaction->AddInputDeletions(compact_->compaction->edit());
  for (size_t i = 0; i < compact_->outputs.size(); i++) {
    const CompactionState::Output& out = compact_->outputs[i];
    compaction->edit()->AddFile(compaction->output_level(), out.number,
                                out.path_id, out.file_size, out.smallest,
                                out.largest, out.smallest_seqno,
                                out.largest_seqno, out.need_compaction);
  }
  return versions_->LogAndApply(compaction->column_family_data(),
                                mutable_cf_options, compaction->edit(),
                                db_mutex, db_directory_);
}

// Given a sequence number, return the sequence number of the
// earliest snapshot that this sequence number is visible in.
// The snapshots themselves are arranged in ascending order of
// sequence numbers.
// Employ a sequential search because the total number of
// snapshots are typically small.
inline SequenceNumber CompactionJob::findEarliestVisibleSnapshot(
    SequenceNumber in, const std::vector<SequenceNumber>& snapshots,
    SequenceNumber* prev_snapshot) {
  assert(snapshots.size());
  SequenceNumber prev __attribute__((unused)) = 0;
  for (const auto cur : snapshots) {
    assert(prev <= cur);
    if (cur >= in) {
      *prev_snapshot = prev;
      return cur;
    }
    prev = cur;  // assignment
    assert(prev);
  }
  Log(InfoLogLevel::WARN_LEVEL, db_options_.info_log,
      "CompactionJob is not able to find snapshot"
      " with SeqId later than %" PRIu64
      ": current MaxSeqId is %" PRIu64 "",
      in, snapshots[snapshots.size() - 1]);
  assert(0);
  return 0;
}

void CompactionJob::RecordCompactionIOStats() {
  RecordTick(stats_, COMPACT_READ_BYTES, IOSTATS(bytes_read));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_READ, IOSTATS(bytes_read));
  IOSTATS_RESET(bytes_read);
  RecordTick(stats_, COMPACT_WRITE_BYTES, IOSTATS(bytes_written));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_WRITTEN, IOSTATS(bytes_written));
  IOSTATS_RESET(bytes_written);
}

Status CompactionJob::OpenCompactionOutputFile() {
  assert(compact_ != nullptr);
  assert(compact_->builder == nullptr);
  // no need to lock because VersionSet::next_file_number_ is atomic
  uint64_t file_number = versions_->NewFileNumber();
  // Make the output file
  std::string fname = TableFileName(db_options_.db_paths, file_number,
                                    compact_->compaction->output_path_id());
  Status s = env_->NewWritableFile(fname, &compact_->outfile, env_options_);

  if (!s.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] OpenCompactionOutputFiles for table #%" PRIu64
        " fails at NewWritableFile with status %s",
        compact_->compaction->column_family_data()->GetName().c_str(), job_id_,
        file_number, s.ToString().c_str());
    LogFlush(db_options_.info_log);
    return s;
  }
  CompactionState::Output out;
  out.number = file_number;
  out.path_id = compact_->compaction->output_path_id();
  out.smallest.Clear();
  out.largest.Clear();
  out.smallest_seqno = out.largest_seqno = 0;

  compact_->outputs.push_back(out);
  compact_->outfile->SetIOPriority(Env::IO_LOW);
  compact_->outfile->SetPreallocationBlockSize(
      static_cast<size_t>(compact_->compaction->OutputFilePreallocationSize()));

  ColumnFamilyData* cfd = compact_->compaction->column_family_data();
  bool skip_filters = false;

  // If the Column family flag is to only optimize filters for hits,
  // we can skip creating filters if this is the bottommost_level where
  // data is going to be found
  //
  if (cfd->ioptions()->optimize_filters_for_hits && bottommost_level_) {
    skip_filters = true;
  }

  compact_->builder.reset(NewTableBuilder(
      *cfd->ioptions(), cfd->internal_comparator(),
      cfd->int_tbl_prop_collector_factories(), compact_->outfile.get(),
      compact_->compaction->output_compression(),
      cfd->ioptions()->compression_opts, skip_filters));
  LogFlush(db_options_.info_log);
  return s;
}

void CompactionJob::CleanupCompaction(const Status& status) {
  if (compact_->builder != nullptr) {
    // May happen if we get a shutdown call in the middle of compaction
    compact_->builder->Abandon();
    compact_->builder.reset();
  } else {
    assert(!status.ok() || compact_->outfile == nullptr);
  }
  for (size_t i = 0; i < compact_->outputs.size(); i++) {
    const CompactionState::Output& out = compact_->outputs[i];

    // If this file was inserted into the table cache then remove
    // them here because this compaction was not committed.
    if (!status.ok()) {
      TableCache::Evict(table_cache_.get(), out.number);
    }
  }
  delete compact_;
  compact_ = nullptr;
}

#ifndef ROCKSDB_LITE
namespace {
void CopyPrefix(
    const Slice& src, size_t prefix_length, std::string* dst) {
  assert(prefix_length > 0);
  size_t length = src.size() > prefix_length ? prefix_length : src.size();
  dst->assign(src.data(), length);
}
}  // namespace

#endif  // !ROCKSDB_LITE

void CompactionJob::UpdateCompactionStats() {
  size_t num_output_files = compact_->outputs.size();
  if (compact_->builder != nullptr) {
    // An error occurred so ignore the last output.
    assert(num_output_files > 0);
    --num_output_files;
  }
  compaction_stats_.num_output_files = static_cast<int>(num_output_files);

  Compaction* compaction = compact_->compaction;
  compaction_stats_.num_input_files_in_non_output_levels = 0;
  compaction_stats_.num_input_files_in_output_level = 0;
  for (int input_level = 0;
       input_level < static_cast<int>(compaction->num_input_levels());
       ++input_level) {
    if (compaction->start_level() + input_level
        != compaction->output_level()) {
      UpdateCompactionInputStatsHelper(
          &compaction_stats_.num_input_files_in_non_output_levels,
          &compaction_stats_.bytes_read_non_output_levels,
          input_level);
    } else {
      UpdateCompactionInputStatsHelper(
          &compaction_stats_.num_input_files_in_output_level,
          &compaction_stats_.bytes_read_output_level,
          input_level);
    }
  }

  for (size_t i = 0; i < num_output_files; i++) {
    compaction_stats_.bytes_written += compact_->outputs[i].file_size;
  }
  if (compact_->num_input_records > compact_->num_output_records) {
    compaction_stats_.num_dropped_records +=
        compact_->num_input_records - compact_->num_output_records;
  }
}

void CompactionJob::UpdateCompactionInputStatsHelper(
    int* num_files, uint64_t* bytes_read, int input_level) {
  const Compaction* compaction = compact_->compaction;
  auto num_input_files = compaction->num_input_files(input_level);
  *num_files += static_cast<int>(num_input_files);

  for (size_t i = 0; i < num_input_files; ++i) {
    const auto* file_meta = compaction->input(input_level, i);
    *bytes_read += file_meta->fd.GetFileSize();
    compaction_stats_.num_input_records +=
        static_cast<uint64_t>(file_meta->num_entries);
  }
}

void CompactionJob::UpdateCompactionJobStats(
    const InternalStats::CompactionStats& stats) const {
#ifndef ROCKSDB_LITE
  if (compaction_job_stats_) {
    compaction_job_stats_->elapsed_micros = stats.micros;

    // input information
    compaction_job_stats_->total_input_bytes =
        stats.bytes_read_non_output_levels +
        stats.bytes_read_output_level;
    compaction_job_stats_->num_input_records =
        compact_->num_input_records;
    compaction_job_stats_->num_input_files =
        stats.num_input_files_in_non_output_levels +
        stats.num_input_files_in_output_level;
    compaction_job_stats_->num_input_files_at_output_level =
        stats.num_input_files_in_output_level;

    // output information
    compaction_job_stats_->total_output_bytes = stats.bytes_written;
    compaction_job_stats_->num_output_records =
        compact_->num_output_records;
    compaction_job_stats_->num_output_files = stats.num_output_files;

    if (compact_->outputs.size() > 0U) {
      CopyPrefix(
          compact_->outputs[0].smallest.user_key(),
          CompactionJobStats::kMaxPrefixLength,
          &compaction_job_stats_->smallest_output_key_prefix);
      CopyPrefix(
          compact_->current_output()->largest.user_key(),
          CompactionJobStats::kMaxPrefixLength,
          &compaction_job_stats_->largest_output_key_prefix);
    }
  }
#endif  // !ROCKSDB_LITE
}

void CompactionJob::LogCompaction(
    ColumnFamilyData* cfd, Compaction* compaction) {
  // Let's check if anything will get logged. Don't prepare all the info if
  // we're not logging
  if (db_options_.info_log_level <= InfoLogLevel::INFO_LEVEL) {
    Compaction::InputLevelSummaryBuffer inputs_summary;
    Log(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
        "[%s] [JOB %d] Compacting %s, score %.2f", cfd->GetName().c_str(),
        job_id_, compaction->InputLevelSummary(&inputs_summary),
        compaction->score());
    char scratch[2345];
    compaction->Summary(scratch, sizeof(scratch));
    Log(InfoLogLevel::INFO_LEVEL, db_options_.info_log,
        "[%s] Compaction start summary: %s\n", cfd->GetName().c_str(), scratch);
    // build event logger report
    auto stream = event_logger_->Log();
    stream << "job" << job_id_ << "event"
           << "compaction_started";
    for (size_t i = 0; i < compaction->num_input_levels(); ++i) {
      stream << ("files_L" + ToString(compaction->level(i)));
      stream.StartArray();
      for (auto f : *compaction->inputs(i)) {
        stream << f->fd.GetNumber();
      }
      stream.EndArray();
    }
    stream << "score" << compaction->score() << "input_data_size"
           << compaction->CalculateTotalInputSize();
  }
}

}  // namespace rocksdb
