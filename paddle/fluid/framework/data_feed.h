/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#pragma once
#if defined _WIN32 || defined __APPLE__
#else
#define _LINUX
#endif

#include <semaphore.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <future>  // NOLINT
#include <memory>
#include <mutex>  // NOLINT
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>  // NOLINT
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "paddle/fluid/framework/archive.h"
#include "paddle/fluid/framework/blocking_queue.h"
#include "paddle/fluid/framework/channel.h"
#include "paddle/fluid/framework/data_feed.pb.h"
#include "paddle/fluid/framework/fleet/fleet_wrapper.h"
#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/reader.h"
#include "paddle/fluid/framework/variable.h"
#include "paddle/fluid/platform/monitor.h"
#include "paddle/fluid/platform/timer.h"
#include "paddle/fluid/string/string_helper.h"

USE_INT_STAT(STAT_total_feasign_num_in_mem);
USE_INT_STAT(STAT_slot_pool_size);
DECLARE_int32(padbox_record_pool_max_size);
DECLARE_int32(padbox_slotpool_thread_num);
DECLARE_int32(padbox_slotrecord_extend_dim);
DECLARE_bool(padbox_auc_runner_mode);
DECLARE_bool(enable_slotrecord_reset_shrink);
DECLARE_bool(enable_slotpool_wait_release);

namespace paddle {
namespace framework {
class DataFeedDesc;
class LoDTensor;
class Scope;
class Variable;
}  // namespace framework
}  // namespace paddle

namespace paddle {
namespace framework {

// DataFeed is the base virtual class for all ohther DataFeeds.
// It is used to read files and parse the data for subsequent trainer.
// Example:
//   DataFeed* reader =
//   paddle::framework::DataFeedFactory::CreateDataFeed(data_feed_name);
//   reader->Init(data_feed_desc); // data_feed_desc is a protobuf object
//   reader->SetFileList(filelist);
//   const std::vector<std::string> & use_slot_alias =
//   reader->GetUseSlotAlias();
//   for (auto name: use_slot_alias){ // for binding memory
//     reader->AddFeedVar(scope->Var(name), name);
//   }
//   reader->Start();
//   while (reader->Next()) {
//      // trainer do something
//   }
HOSTDEVICE union FeatureKey {
  uint64_t uint64_feasign_;
  float float_feasign_;
};

#ifdef PADDLE_WITH_CUDA
#define PADDLE_ALIGN(x) __attribute__((aligned(x)))
#else
#define PADDLE_ALIGN(x)
#endif

struct PADDLE_ALIGN(8) FeatureItem {
  FeatureItem() {}
  FeatureItem(FeatureKey sign, uint16_t slot) {
    this->sign() = sign;
    this->slot() = slot;
  }
  HOSTDEVICE FeatureKey& sign() {
    return *(reinterpret_cast<FeatureKey*>(sign_buffer()));
  }
  HOSTDEVICE const FeatureKey& sign() const {
    const FeatureKey* ret = reinterpret_cast<FeatureKey*>(sign_buffer());
    return *ret;
  }
  uint16_t& slot() { return slot_; }
  const uint16_t& slot() const { return slot_; }

 private:
  HOSTDEVICE char* sign_buffer() const { return const_cast<char*>(sign_); }
  char sign_[sizeof(FeatureKey)];
  uint16_t slot_;
  uint16_t slot_2;
  uint16_t slot_3;
  uint16_t slot_4;
};

// sizeof Record is much less than std::vector<MultiSlotType>
struct Record {
  std::vector<FeatureItem> uint64_feasigns_;
  std::vector<FeatureItem> float_feasigns_;
  std::string ins_id_;
  std::string content_;
  uint64_t search_id;
  uint32_t rank;
  uint32_t cmatch;
};

struct PvInstanceObject {
  std::vector<Record*> ads;
  void merge_instance(Record* ins) { ads.push_back(ins); }
};

using PvInstance = PvInstanceObject*;

inline PvInstance make_pv_instance() { return new PvInstanceObject(); }

class DataFeed {
 public:
  DataFeed() {
    mutex_for_pick_file_ = nullptr;
    file_idx_ = nullptr;
    mutex_for_fea_num_ = nullptr;
    total_fea_num_ = nullptr;
  }
  virtual ~DataFeed() {}
  virtual void Init(const DataFeedDesc& data_feed_desc) = 0;
  virtual bool CheckFile(const char* filename) {
    PADDLE_THROW(platform::errors::Unimplemented(
        "This function(CheckFile) is not implemented."));
  }
  // Set filelist for DataFeed.
  // Pay attention that it must init all readers before call this function.
  // Otherwise, Init() function will init finish_set_filelist_ flag.
  virtual bool SetFileList(const std::vector<std::string>& files);
  virtual bool Start() = 0;

  // The trainer calls the Next() function, and the DataFeed will load a new
  // batch to the feed_vec. The return value of this function is the batch
  // size of the current batch.
  virtual int Next() = 0;
  // Get all slots' alias which defined in protofile
  virtual const std::vector<std::string>& GetAllSlotAlias() {
    return all_slots_;
  }
  // Get used slots' alias which defined in protofile
  virtual const std::vector<std::string>& GetUseSlotAlias() {
    return use_slots_;
  }
  // This function is used for binding feed_vec memory
  virtual void AddFeedVar(Variable* var, const std::string& name);

  // This function is used for binding feed_vec memory in a given scope
  virtual void AssignFeedVar(const Scope& scope);

  // This function will do nothing at default
  virtual void SetInputPvChannel(void* channel) {}
  // This function will do nothing at default
  virtual void SetOutputPvChannel(void* channel) {}
  // This function will do nothing at default
  virtual void SetConsumePvChannel(void* channel) {}

  // This function will do nothing at default
  virtual void SetInputChannel(void* channel) {}
  virtual void SetInputPtrChannel(void* channel) {}
  // This function will do nothing at default
  virtual void SetOutputChannel(void* channel) {}
  virtual void SetOutputPtrChannel(void* channel) {}
  // This function will do nothing at default
  virtual void SetConsumeChannel(void* channel) {}
  virtual void SetConsumePtrChannel(void* channel) {}
  // This function will do nothing at default
  virtual void SetThreadId(int thread_id) {}
  // This function will do nothing at default
  virtual void SetThreadNum(int thread_num) {}
  // This function will do nothing at default
  virtual void SetParseInsId(bool parse_ins_id) {}
  virtual void SetParseContent(bool parse_content) {}
  virtual void SetParseLogKey(bool parse_logkey) {}
  virtual void SetEnablePvMerge(bool enable_pv_merge) {}
  virtual void SetCurrentPhase(int current_phase) {}
  virtual void SetFileListMutex(std::mutex* mutex) {
    mutex_for_pick_file_ = mutex;
  }
  virtual void SetFeaNumMutex(std::mutex* mutex) { mutex_for_fea_num_ = mutex; }
  virtual void SetFileListIndex(size_t* file_index) { file_idx_ = file_index; }
  virtual void SetFeaNum(uint64_t* fea_num) { total_fea_num_ = fea_num; }
  virtual const std::string& GetLineId(int idx) const {
    return ins_id_vec_[idx];
  }
  virtual const std::string& GetContent(int idx) const {
    return ins_content_vec_[idx];
  }
  virtual int GetCurBatchSize() {
    return std::max(batch_size_, static_cast<int>(ins_id_vec_.size()));
  }
  virtual void LoadIntoMemory() {
    PADDLE_THROW(platform::errors::Unimplemented(
        "This function(LoadIntoMemory) is not implemented."));
  }
  virtual void SetPlace(const paddle::platform::Place& place) {
    place_ = place;
  }
  virtual const paddle::platform::Place& GetPlace() const { return place_; }
  virtual void SetSampleRate(float r) { sample_rate_ = r; }
  virtual void SetLoadArchiveFile(bool archive) { is_archive_file_ = archive; }

 protected:
  // The following three functions are used to check if it is executed in this
  // order:
  //   Init() -> SetFileList() -> Start() -> Next()
  virtual void CheckInit();
  virtual void CheckSetFileList();
  virtual void CheckStart();
  virtual void SetBatchSize(
      int batch);  // batch size will be set in Init() function
  // This function is used to pick one file from the global filelist(thread
  // safe).
  virtual bool PickOneFile(std::string* filename);
  virtual void CopyToFeedTensor(void* dst, const void* src, size_t size);

  std::vector<std::string> filelist_;
  size_t* file_idx_;
  std::mutex* mutex_for_pick_file_;
  std::mutex* mutex_for_fea_num_ = nullptr;
  uint64_t* total_fea_num_ = nullptr;
  uint64_t fea_num_ = 0;

  // the alias of used slots, and its order is determined by
  // data_feed_desc(proto object)
  std::vector<std::string> use_slots_;
  std::vector<bool> use_slots_is_dense_;

  // the alias of all slots, and its order is determined by data_feed_desc(proto
  // object)
  std::vector<std::string> all_slots_;
  std::vector<std::string> all_slots_type_;
  std::vector<std::vector<int>> use_slots_shape_;
  std::vector<int> inductive_shape_index_;
  std::vector<int> total_dims_without_inductive_;
  // For the inductive shape passed within data
  std::vector<std::vector<int>> multi_inductive_shape_index_;
  std::vector<int>
      use_slots_index_;  // -1: not used; >=0: the index of use_slots_

  // The data read by DataFeed will be stored here
  std::vector<LoDTensor*> feed_vec_;

  LoDTensor* rank_offset_;

  // the batch size defined by user
  int default_batch_size_;
  // current batch size
  int batch_size_;

  bool finish_init_;
  bool finish_set_filelist_;
  bool finish_start_;
  std::string pipe_command_;
  std::vector<std::string> ins_id_vec_;
  std::vector<std::string> ins_content_vec_;
  platform::Place place_;

  // The input type of pipe reader, 0 for one sample, 1 for one batch
  int input_type_;
  float sample_rate_ = 1.0f;
  bool is_archive_file_ = false;
};

// PrivateQueueDataFeed is the base virtual class for ohther DataFeeds.
// It use a read-thread to read file and parse data to a private-queue
// (thread level), and get data from this queue when trainer call Next().
template <typename T>
class PrivateQueueDataFeed : public DataFeed {
 public:
  PrivateQueueDataFeed() {}
  virtual ~PrivateQueueDataFeed() {}
  virtual bool Start();
  virtual int Next();

 protected:
  // The thread implementation function for reading file and parse.
  virtual void ReadThread();
  // This function is used to set private-queue size, and the most
  // efficient when the queue size is close to the batch size.
  virtual void SetQueueSize(int queue_size);
  // The reading and parsing method called in the ReadThread.
  virtual bool ParseOneInstance(T* instance) = 0;
  virtual bool ParseOneInstanceFromPipe(T* instance) = 0;
  // This function is used to put instance to vec_ins
  virtual void AddInstanceToInsVec(T* vec_ins, const T& instance,
                                   int index) = 0;
  // This function is used to put ins_vec to feed_vec
  virtual void PutToFeedVec(const T& ins_vec) = 0;

  // The thread for read files
  std::thread read_thread_;
  // using ifstream one line and one line parse is faster
  // than using fread one buffer and one buffer parse.
  //   for a 601M real data:
  //     ifstream one line and one line parse: 6034 ms
  //     fread one buffer and one buffer parse: 7097 ms
  std::ifstream file_;
  std::shared_ptr<FILE> fp_;
  size_t queue_size_;
  string::LineFileReader reader_;
  // The queue for store parsed data
  std::shared_ptr<paddle::framework::ChannelObject<T>> queue_;
};

template <typename T>
class InMemoryDataFeed : public DataFeed {
 public:
  InMemoryDataFeed();
  virtual ~InMemoryDataFeed() {}
  virtual void Init(const DataFeedDesc& data_feed_desc) = 0;
  virtual bool Start();
  virtual int Next();
  virtual void SetInputPvChannel(void* channel);
  virtual void SetOutputPvChannel(void* channel);
  virtual void SetConsumePvChannel(void* channel);

  virtual void SetInputChannel(void* channel);
  virtual void SetOutputChannel(void* channel);
  virtual void SetConsumeChannel(void* channel);
  virtual void SetInputPtrChannel(void* channel);
  virtual void SetOutputPtrChannel(void* channel);
  virtual void SetConsumePtrChannel(void* channel);
  virtual void SetThreadId(int thread_id);
  virtual void SetThreadNum(int thread_num);
  virtual void SetParseInsId(bool parse_ins_id);
  virtual void SetParseContent(bool parse_content);
  virtual void SetParseLogKey(bool parse_logkey);
  virtual void SetEnablePvMerge(bool enable_pv_merge);
  virtual void SetCurrentPhase(int current_phase);
  virtual void LoadIntoMemory();

 protected:
  virtual bool ParseOneInstance(T* instance) = 0;
  virtual bool ParseOneInstanceFromPipe(T* instance) = 0;
  virtual bool ParseOneInstanceFromPipe(Record* instance,
                                        const std::string& line) = 0;
  virtual void PutToFeedVec(const std::vector<T>& ins_vec) = 0;

  int thread_id_;
  int thread_num_;
  bool parse_ins_id_;
  bool parse_content_;
  bool parse_logkey_;
  bool enable_pv_merge_;
  int current_phase_{-1};  // only for untest
  std::ifstream file_;
  std::shared_ptr<FILE> fp_;
  paddle::framework::ChannelObject<T>* input_channel_;
  paddle::framework::ChannelObject<T>* output_channel_;
  paddle::framework::ChannelObject<T>* consume_channel_;

  paddle::framework::ChannelObject<T*>* input_ptr_channel_;
  paddle::framework::ChannelObject<T*>* output_ptr_channel_;
  paddle::framework::ChannelObject<T*>* consume_ptr_channel_;

  paddle::framework::ChannelObject<PvInstance>* input_pv_channel_;
  paddle::framework::ChannelObject<PvInstance>* output_pv_channel_;
  paddle::framework::ChannelObject<PvInstance>* consume_pv_channel_;
  std::vector<T*> ins_vec_;
};

// This class define the data type of instance(ins_vec) in MultiSlotDataFeed
class MultiSlotType {
 public:
  MultiSlotType() {}
  ~MultiSlotType() {}
  void Init(const std::string& type, size_t reserved_size = 0) {
    CheckType(type);
    if (type_[0] == 'f') {
      float_feasign_.clear();
      if (reserved_size) {
        float_feasign_.reserve(reserved_size);
      }
    } else if (type_[0] == 'u') {
      uint64_feasign_.clear();
      if (reserved_size) {
        uint64_feasign_.reserve(reserved_size);
      }
    }
    type_ = type;
  }
  void InitOffset(size_t max_batch_size = 0) {
    if (max_batch_size > 0) {
      offset_.reserve(max_batch_size + 1);
    }
    offset_.resize(1);
    // LoDTensor' lod is counted from 0, the size of lod
    // is one size larger than the size of data.
    offset_[0] = 0;
  }
  const std::vector<size_t>& GetOffset() const { return offset_; }
  std::vector<size_t>& MutableOffset() { return offset_; }
  void AddValue(const float v) {
    CheckFloat();
    float_feasign_.push_back(v);
  }
  void AddValue(const uint64_t v) {
    CheckUint64();
    uint64_feasign_.push_back(v);
  }
  void CopyValues(const float* input, size_t size) {
    CheckFloat();
    float_feasign_.resize(size);
    memcpy(float_feasign_.data(), input, size * sizeof(float));
  }
  void CopyValues(const uint64_t* input, size_t size) {
    CheckUint64();
    uint64_feasign_.resize(size);
    memcpy(uint64_feasign_.data(), input, size * sizeof(uint64_t));
  }
  void AddIns(const MultiSlotType& ins) {
    if (ins.GetType()[0] == 'f') {  // float
      CheckFloat();
      auto& vec = ins.GetFloatData();
      offset_.push_back(offset_.back() + vec.size());
      float_feasign_.insert(float_feasign_.end(), vec.begin(), vec.end());
    } else if (ins.GetType()[0] == 'u') {  // uint64
      CheckUint64();
      auto& vec = ins.GetUint64Data();
      offset_.push_back(offset_.back() + vec.size());
      uint64_feasign_.insert(uint64_feasign_.end(), vec.begin(), vec.end());
    }
  }
  void AppendValues(const uint64_t* input, size_t size) {
    CheckUint64();
    offset_.push_back(offset_.back() + size);
    uint64_feasign_.insert(uint64_feasign_.end(), input, input + size);
  }
  void AppendValues(const float* input, size_t size) {
    CheckFloat();
    offset_.push_back(offset_.back() + size);
    float_feasign_.insert(float_feasign_.end(), input, input + size);
  }
  const std::vector<float>& GetFloatData() const { return float_feasign_; }
  std::vector<float>& MutableFloatData() { return float_feasign_; }
  const std::vector<uint64_t>& GetUint64Data() const { return uint64_feasign_; }
  std::vector<uint64_t>& MutableUint64Data() { return uint64_feasign_; }
  const std::string& GetType() const { return type_; }
  size_t GetBatchSize() { return offset_.size() - 1; }
  std::string& MutableType() { return type_; }

  std::string DebugString() {
    std::stringstream ss;

    ss << "\ntype: " << type_ << "\n";
    ss << "offset: ";
    ss << "[";
    for (const size_t& i : offset_) {
      ss << offset_[i] << ",";
    }
    ss << "]\ndata: [";
    if (type_[0] == 'f') {
      for (const float& i : float_feasign_) {
        ss << i << ",";
      }
    } else {
      for (const uint64_t& i : uint64_feasign_) {
        ss << i << ",";
      }
    }
    ss << "]\n";
    return ss.str();
  }

 private:
  void CheckType(const std::string& type) const {
    PADDLE_ENFORCE_EQ((type == "uint64" || type == "float"), true,
                      platform::errors::InvalidArgument(
                          "MultiSlotType error, expect type is uint64 or "
                          "float, but received type is %s.",
                          type));
  }
  void CheckFloat() const {
    PADDLE_ENFORCE_EQ(
        type_[0], 'f',
        platform::errors::InvalidArgument(
            "MultiSlotType error, add %s value to float slot.", type_));
  }
  void CheckUint64() const {
    PADDLE_ENFORCE_EQ(
        type_[0], 'u',
        platform::errors::InvalidArgument(
            "MultiSlotType error, add %s value to uint64 slot.", type_));
  }
  std::vector<float> float_feasign_;
  std::vector<uint64_t> uint64_feasign_;
  std::string type_;
  std::vector<size_t> offset_;
};

template <class AR>
paddle::framework::Archive<AR>& operator<<(paddle::framework::Archive<AR>& ar,
                                           const MultiSlotType& ins) {
  ar << ins.GetType();
#ifdef _LINUX
  ar << ins.GetOffset();
#else
  const auto& offset = ins.GetOffset();
  ar << (uint64_t)offset.size();
  for (const size_t& x : offset) {
    ar << (const uint64_t)x;
  }
#endif
  ar << ins.GetFloatData();
  ar << ins.GetUint64Data();
  return ar;
}

template <class AR>
paddle::framework::Archive<AR>& operator>>(paddle::framework::Archive<AR>& ar,
                                           MultiSlotType& ins) {
  ar >> ins.MutableType();
#ifdef _LINUX
  ar >> ins.MutableOffset();
#else
  auto& offset = ins.MutableOffset();
  offset.resize(ar.template Get<uint64_t>());
  for (size_t& x : offset) {
    uint64_t t;
    ar >> t;
    x = (size_t)t;
  }
#endif
  ar >> ins.MutableFloatData();
  ar >> ins.MutableUint64Data();
  return ar;
}

struct RecordCandidate {
  std::string ins_id_;
  std::unordered_multimap<uint16_t, FeatureKey> feas_;
  size_t shadow_index_ = -1;  // Optimization for Reservoir Sample

  RecordCandidate() {}
  RecordCandidate(const Record& rec,
                  const std::unordered_set<uint16_t>& slot_index_to_replace) {
    for (const auto& fea : rec.uint64_feasigns_) {
      if (slot_index_to_replace.find(fea.slot()) !=
          slot_index_to_replace.end()) {
        feas_.insert({fea.slot(), fea.sign()});
      }
    }
  }

  RecordCandidate& operator=(const Record& rec) {
    feas_.clear();
    ins_id_ = rec.ins_id_;
    for (auto& fea : rec.uint64_feasigns_) {
      feas_.insert({fea.slot(), fea.sign()});
    }
    return *this;
  }
};

class RecordCandidateList {
 public:
  RecordCandidateList() = default;
  RecordCandidateList(const RecordCandidateList&) {}

  size_t Size() { return cur_size_; }
  void ReSize(size_t length);

  void ReInit();
  void ReInitPass() {
    mutex_.lock();
    for (size_t i = 0; i < cur_size_; ++i) {
      if (candidate_list_[i].shadow_index_ != i) {
        candidate_list_[i].ins_id_ =
            candidate_list_[candidate_list_[i].shadow_index_].ins_id_;
        candidate_list_[i].feas_.swap(
            candidate_list_[candidate_list_[i].shadow_index_].feas_);
        candidate_list_[i].shadow_index_ = i;
      }
    }
    candidate_list_.resize(capacity_);
    mutex_.unlock();
  }

  void AddAndGet(const Record& record, RecordCandidate* result);
  void AddAndGet(const Record& record, size_t& index_result) {  // NOLINT
    // std::unique_lock<std::mutex> lock(mutex_);
    size_t index = 0;
    ++total_size_;
    auto fleet_ptr = FleetWrapper::GetInstance();
    if (!full_) {
      // candidate_list_.emplace_back(record, slot_index_to_replace_);
      candidate_list_[cur_size_] =
          RecordCandidate(record, slot_index_to_replace_);
      // candidate_list_.back().shadow_index_ = cur_size_;
      candidate_list_[cur_size_].shadow_index_ = cur_size_;
      ++cur_size_;
      full_ = (cur_size_ == capacity_);
    } else {
      index = fleet_ptr->LocalRandomEngine()() % total_size_;
      if (index < capacity_) {
        candidate_list_.emplace_back(record, slot_index_to_replace_);
        candidate_list_[index].shadow_index_ = candidate_list_.size() - 1;
      }
    }
    index = fleet_ptr->LocalRandomEngine()() % cur_size_;
    index_result = candidate_list_[index].shadow_index_;
  }
  const RecordCandidate& Get(size_t index) const {
    PADDLE_ENFORCE_LT(
        index, candidate_list_.size(),
        platform::errors::OutOfRange("Your index [%lu] exceeds the number of "
                                     "elements in candidate_list[%lu].",
                                     index, candidate_list_.size()));
    return candidate_list_[index];
  }
  void SetSlotIndexToReplace(
      const std::unordered_set<uint16_t>& slot_index_to_replace) {
    slot_index_to_replace_ = slot_index_to_replace;
  }

 private:
  size_t capacity_ = 0;
  std::mutex mutex_;
  bool full_ = false;
  size_t cur_size_ = 0;
  size_t total_size_ = 0;
  std::vector<RecordCandidate> candidate_list_;
  std::unordered_set<uint16_t> slot_index_to_replace_;
};

template <class AR>
paddle::framework::Archive<AR>& operator<<(paddle::framework::Archive<AR>& ar,
                                           const FeatureKey& fk) {
  ar << fk.uint64_feasign_;
  ar << fk.float_feasign_;
  return ar;
}

template <class AR>
paddle::framework::Archive<AR>& operator>>(paddle::framework::Archive<AR>& ar,
                                           FeatureKey& fk) {
  ar >> fk.uint64_feasign_;
  ar >> fk.float_feasign_;
  return ar;
}

template <class AR>
paddle::framework::Archive<AR>& operator<<(paddle::framework::Archive<AR>& ar,
                                           const FeatureItem& fi) {
  ar << fi.sign();
  ar << fi.slot();
  return ar;
}

template <class AR>
paddle::framework::Archive<AR>& operator>>(paddle::framework::Archive<AR>& ar,
                                           FeatureItem& fi) {
  ar >> fi.sign();
  ar >> fi.slot();
  return ar;
}

template <class AR>
paddle::framework::Archive<AR>& operator<<(paddle::framework::Archive<AR>& ar,
                                           const Record& r) {
  ar << r.uint64_feasigns_;
  ar << r.float_feasigns_;
  ar << r.ins_id_;
  return ar;
}

template <class AR>
paddle::framework::Archive<AR>& operator>>(paddle::framework::Archive<AR>& ar,
                                           Record& r) {
  ar >> r.uint64_feasigns_;
  ar >> r.float_feasigns_;
  ar >> r.ins_id_;
  return ar;
}

// This DataFeed is used to feed multi-slot type data.
// The format of multi-slot type data:
//   [n feasign_0 feasign_1 ... feasign_n]*
class MultiSlotDataFeed
    : public PrivateQueueDataFeed<std::vector<MultiSlotType>> {
 public:
  MultiSlotDataFeed() {}
  virtual ~MultiSlotDataFeed() {}
  virtual void Init(const DataFeedDesc& data_feed_desc);
  virtual bool CheckFile(const char* filename);

 protected:
  virtual void ReadThread();
  virtual void AddInstanceToInsVec(std::vector<MultiSlotType>* vec_ins,
                                   const std::vector<MultiSlotType>& instance,
                                   int index);
  virtual bool ParseOneInstance(std::vector<MultiSlotType>* instance);
  virtual bool ParseOneInstanceFromPipe(std::vector<MultiSlotType>* instance);
  virtual void PutToFeedVec(const std::vector<MultiSlotType>& ins_vec);
};

class MultiSlotInMemoryDataFeed : public InMemoryDataFeed<Record> {
 public:
  MultiSlotInMemoryDataFeed() {}
  virtual ~MultiSlotInMemoryDataFeed() {}
  virtual void Init(const DataFeedDesc& data_feed_desc);
#ifdef PADDLE_WITH_CUDA
  virtual void CopyForTensor(const paddle::platform::Place& place,
                             FeatureItem* src, void** dest, size_t* offset,
                             char* type, size_t total_size, size_t row_size,
                             size_t col_size);
#endif

 protected:
  virtual bool ParseOneInstance(Record* instance);
  virtual bool ParseOneInstanceFromPipe(Record* instance);
  virtual bool ParseOneInstanceFromPipe(Record* instance,
                                        const std::string& line);
  virtual void PutToFeedVec(const std::vector<Record>& ins_vec);
  virtual void GetMsgFromLogKey(const std::string& log_key, uint64_t* search_id,
                                uint32_t* cmatch, uint32_t* rank);
  std::vector<std::vector<float>> batch_float_feasigns_;
  std::vector<std::vector<uint64_t>> batch_uint64_feasigns_;
  std::vector<std::vector<size_t>> offset_;
  std::vector<bool> visit_;
  std::vector<size_t> offset;
  std::vector<size_t> offset_sum;
};

class PaddleBoxDataFeed : public MultiSlotInMemoryDataFeed {
 public:
  PaddleBoxDataFeed() {}
  virtual ~PaddleBoxDataFeed() {}

 protected:
  virtual void Init(const DataFeedDesc& data_feed_desc);
  virtual bool Start();
  virtual int Next();
  virtual void AssignFeedVar(const Scope& scope);
  virtual void PutToFeedVec(const std::vector<PvInstance>& pv_vec);
  virtual void PutToFeedVec(const std::vector<Record*>& ins_vec);
  virtual int GetCurrentPhase();
  virtual void GetRankOffset(const std::vector<PvInstance>& pv_vec,
                             int ins_number);
  std::string rank_offset_name_;
  int pv_batch_size_;
  std::vector<PvInstance> pv_vec_;
};

#ifdef PADDLE_WITH_BOX_PS
template <typename T>
struct SlotValues {
  std::vector<T> slot_values;
  std::vector<uint32_t> slot_offsets;

  void add_values(const T* values, uint32_t num) {
    if (slot_offsets.empty()) {
      slot_offsets.push_back(0);
    }
    if (num > 0) {
      slot_values.insert(slot_values.end(), values, values + num);
    }
    slot_offsets.push_back(static_cast<uint32_t>(slot_values.size()));
  }
  T* get_values(int idx, size_t* size) {
    uint32_t& offset = slot_offsets[idx];
    (*size) = slot_offsets[idx + 1] - offset;
    return &slot_values[offset];
  }
  void add_slot_feasigns(const std::vector<std::vector<T>>& slot_feasigns,
                         uint32_t fea_num) {
    slot_values.reserve(fea_num);
    int slot_num = static_cast<int>(slot_feasigns.size());
    slot_offsets.resize(slot_num + 1);
    for (int i = 0; i < slot_num; ++i) {
      auto& slot_val = slot_feasigns[i];
      slot_offsets[i] = static_cast<uint32_t>(slot_values.size());
      uint32_t num = static_cast<uint32_t>(slot_val.size());
      if (num > 0) {
        slot_values.insert(slot_values.end(), slot_val.begin(), slot_val.end());
      }
    }
    slot_offsets[slot_num] = slot_values.size();
  }
  void clear(bool shrink) {
    slot_offsets.clear();
    slot_values.clear();
    if (shrink) {
      slot_values.shrink_to_fit();
      slot_offsets.shrink_to_fit();
    }
  }
};

struct AucRunnerInfo {
  size_t record_id_;
  size_t pool_id_;
  size_t replaced_id_;
};

// sizeof Record is much less than std::vector<MultiSlotType>
struct SlotRecordObject {
  uint64_t search_id;
  uint32_t rank;
  uint32_t cmatch;
  std::string ins_id_;
  SlotValues<uint64_t> slot_uint64_feasigns_;
  SlotValues<float> slot_float_feasigns_;

  ~SlotRecordObject() { clear(true); }
  void reset(void) { clear(FLAGS_enable_slotrecord_reset_shrink); }
  void clear(bool shrink) {
    slot_uint64_feasigns_.clear(shrink);
    slot_float_feasigns_.clear(shrink);
  }
  void debug(void) {
    VLOG(0) << "ins:" << ins_id_
            << ", uint64:" << slot_uint64_feasigns_.slot_values.size()
            << ", float:" << slot_float_feasigns_.slot_values.size();
  }
};
using SlotRecord = SlotRecordObject*;

inline SlotRecord make_slotrecord() {
  static const size_t slot_record_byte_size =
      sizeof(SlotRecordObject) +
      sizeof(float) * FLAGS_padbox_slotrecord_extend_dim +
      sizeof(AucRunnerInfo) * static_cast<int>(FLAGS_padbox_auc_runner_mode);
  void* p = malloc(slot_record_byte_size);
  new (p) SlotRecordObject;
  return reinterpret_cast<SlotRecordObject*>(p);
}

inline AucRunnerInfo* get_auc_runner_info(SlotRecord record) {
  auto offset = sizeof(SlotRecordObject) +
                sizeof(float) * FLAGS_padbox_slotrecord_extend_dim;
  return reinterpret_cast<AucRunnerInfo*>(
      &(reinterpret_cast<char*>(record)[offset]));
}

inline void free_slotrecord(SlotRecordObject* p) {
  p->~SlotRecordObject();
  free(p);
}

struct SlotPvInstanceObject {
  std::vector<SlotRecord> ads;
  ~SlotPvInstanceObject() {
    ads.clear();
    ads.shrink_to_fit();
  }
  void merge_instance(SlotRecord ins) { ads.push_back(ins); }
};

using SlotPvInstance = SlotPvInstanceObject*;
inline SlotPvInstance make_slotpv_instance() {
  return new SlotPvInstanceObject();
}

inline int GetTotalFeaNum(const std::vector<SlotRecord>& slot_record,
                          size_t len) {
  int num = 0;
  size_t size = len < slot_record.size() ? len : slot_record.size();
  for (size_t i = 0; i < size; ++i) {
    num += slot_record[i]->slot_uint64_feasigns_.slot_values.capacity();
  }
  return num;
}

template <class T>
class SlotObjAllocator {
 public:
  explicit SlotObjAllocator(std::function<void(T*)> deleter)
      : free_nodes_(NULL), capacity_(0), deleter_(deleter) {}
  ~SlotObjAllocator() { clear(); }

  void clear() {
    T* tmp = NULL;
    while (free_nodes_ != NULL) {
      tmp = reinterpret_cast<T*>(reinterpret_cast<void*>(free_nodes_));
      free_nodes_ = free_nodes_->next;
      deleter_(tmp);
      --capacity_;
    }
    CHECK_EQ(capacity_, static_cast<size_t>(0));
  }
  T* acquire(void) {
    T* x = NULL;
    x = reinterpret_cast<T*>(reinterpret_cast<void*>(free_nodes_));
    free_nodes_ = free_nodes_->next;
    --capacity_;
    return x;
  }
  void release(T* x) {
    Node* node = reinterpret_cast<Node*>(reinterpret_cast<void*>(x));
    node->next = free_nodes_;
    free_nodes_ = node;
    ++capacity_;
  }
  size_t capacity(void) { return capacity_; }

 private:
  struct alignas(T) Node {
    union {
      Node* next;
      char data[sizeof(T)];
    };
  };
  Node* free_nodes_;  // a list
  size_t capacity_;
  std::function<void(T*)> deleter_ = nullptr;
};
static const int OBJPOOL_BLOCK_SIZE = 10000;
class SlotObjPool {
 public:
  SlotObjPool()
      : max_capacity_(FLAGS_padbox_record_pool_max_size),
        alloc_(free_slotrecord) {
    ins_chan_ = MakeChannel<SlotRecord>();
    ins_chan_->SetBlockSize(OBJPOOL_BLOCK_SIZE);
    for (int i = 0; i < FLAGS_padbox_slotpool_thread_num; ++i) {
      threads_.push_back(std::thread([this]() { run(); }));
    }
    disable_pool_ = false;
    count_ = 0;
  }
  ~SlotObjPool() {
    ins_chan_->Close();
    for (auto& t : threads_) {
      t.join();
    }
  }
  void disable_pool(bool disable) { disable_pool_ = disable; }
  void set_max_capacity(size_t max_capacity) { max_capacity_ = max_capacity; }
  void get(std::vector<SlotRecord>* output, int n) {
    output->resize(n);
    return get(&(*output)[0], n);
  }
  void get(SlotRecord* output, int n) {
    int size = 0;
    mutex_.lock();
    int left = static_cast<int>(alloc_.capacity());
    if (left > 0) {
      size = (left >= n) ? n : left;
      for (int i = 0; i < size; ++i) {
        output[i] = alloc_.acquire();
      }
    }
    mutex_.unlock();
    count_ += n;
    if (size == n) {
      return;
    }
    for (int i = size; i < n; ++i) {
      output[i] = make_slotrecord();
    }
  }
  void put(std::vector<SlotRecord>* input) {
    size_t size = input->size();
    if (size == 0) {
      return;
    }
    put(&(*input)[0], size);
    input->clear();
  }
  void put(SlotRecord* input, size_t size) {
    CHECK(ins_chan_->WriteMove(size, input) == size);
  }
  void run(void) {
    std::vector<SlotRecord> input;
    while (ins_chan_->ReadOnce(input, OBJPOOL_BLOCK_SIZE)) {
      if (input.empty()) {
        continue;
      }
      // over max capacity
      size_t n = input.size();
      count_ -= n;
      if (disable_pool_ || n + capacity() > max_capacity_) {
        for (auto& t : input) {
          free_slotrecord(t);
        }
      } else {
        for (auto& t : input) {
          t->reset();
        }
        mutex_.lock();
        for (auto& t : input) {
          alloc_.release(t);
        }
        mutex_.unlock();
      }
      input.clear();
    }
  }
  void clear(void) {
    platform::Timer timeline;
    timeline.Start();
    mutex_.lock();
    alloc_.clear();
    mutex_.unlock();
    // wait release channel data
    if (FLAGS_enable_slotpool_wait_release) {
      while (!ins_chan_->Empty()) {
        sleep(1);
      }
    }
    timeline.Pause();
    LOG(WARNING) << "clear slot pool data size=" << count_.load()
                 << ", span=" << timeline.ElapsedSec();
  }
  size_t capacity(void) {
    mutex_.lock();
    size_t total = alloc_.capacity();
    mutex_.unlock();
    return total;
  }

 private:
  size_t max_capacity_;
  Channel<SlotRecord> ins_chan_;
  std::vector<std::thread> threads_;
  std::mutex mutex_;
  SlotObjAllocator<SlotRecordObject> alloc_;
  bool disable_pool_;
  std::atomic<long> count_;  // NOLINT
};

inline SlotObjPool& SlotRecordPool() {
  static SlotObjPool pool;
  return pool;
}
// down disk pool
inline SlotObjPool& SlotRecordDownPool() {
  static SlotObjPool pool;
  return pool;
}

using FeasignValues = SlotValues<uint64_t>;

struct FeasignValuesCandidate {
  // slot -> slot_values
  std::unordered_map<uint16_t, std::vector<uint64_t>> feasign_values_;
  std::vector<FeasignValuesCandidate>* candidates_v_ = nullptr;
  size_t offset_ = 0;
  size_t replaced_idx_ = 0;

  FeasignValuesCandidate() {}

  FeasignValuesCandidate(const FeasignValues& values,
                         const std::set<uint16_t>& slots_idx) {
    for (auto idx : slots_idx) {
      const auto& begin = values.slot_values.begin() + values.slot_offsets[idx];
      const auto& end =
          values.slot_values.begin() + values.slot_offsets[idx + 1];
      feasign_values_.emplace(idx, std::vector<uint64_t>(begin, end));
    }
  }
};

class FeasignValuesCandidateList {
 public:
  FeasignValuesCandidateList() = default;
  FeasignValuesCandidateList(const FeasignValuesCandidateList&) {}

  size_t Size() { return size_; }
  bool Full() { return size_ == capacity_; }

  void Resize(size_t len) {
    capacity_ = len;
    id_ = 0;
    size_ = 0;

    candidates_.resize(capacity_);

    new_candidates_q_.clear();
    candidates_cache_.clear();
  }

  void SetReplacedSlots(const std::set<uint16_t>& replaced_slots) {
    replaced_slots_ = replaced_slots;
  }

  size_t AddAndGet(const FeasignValues& values) {
    size_t replaced_id = 0;
    auto& random = FleetWrapper::GetInstance()->LocalRandomEngine();

    ++id_;
    if (!Full()) {
      candidates_[size_] = FeasignValuesCandidate(values, replaced_slots_);
      ++size_;
    } else {
      replaced_id = random() % id_;
      if (replaced_id < capacity_) {
        size_t cur_offset = new_candidates_q_.back().size();
        new_candidates_q_.back().emplace_back(values, replaced_slots_);
        new_candidates_q_.back()[cur_offset].replaced_idx_ = replaced_id;

        candidates_[replaced_id].candidates_v_ = &new_candidates_q_.back();
        candidates_[replaced_id].offset_ = cur_offset;
      }
    }

    replaced_id = random() % size_;
    if (candidates_[replaced_id].candidates_v_ == &new_candidates_q_.back()) {
      replaced_id = size_ + candidates_[replaced_id].offset_;
    }

    return replaced_id;
  }

  FeasignValuesCandidate& GetUseId(size_t id) {
    CHECK(id < candidates_.size());
    if (candidates_[id].candidates_v_ == nullptr) {
      return candidates_[id];
    }
    return candidates_[id].candidates_v_->at(candidates_[id].offset_);
  }

  FeasignValuesCandidate& GetUseReplaceId(size_t replaced_id) {
    if (replaced_id >= capacity_) {
      CHECK(replaced_id - capacity_ < new_candidates_q_.front().size());
      return new_candidates_q_.front()[replaced_id - capacity_];
    }

    return candidates_[replaced_id];
  }

  void Push() {
    std::lock_guard<std::mutex> lock(mutex4cache_);

    if (!candidates_cache_.empty()) {
      candidates_cache_.front().clear();
      new_candidates_q_.push_back(std::move(candidates_cache_.front()));
      candidates_cache_.pop_front();
    } else {
      new_candidates_q_.push_back({});
    }
  }

  void Pop() {
    for (auto& c : new_candidates_q_.front()) {
      if (candidates_[c.replaced_idx_].candidates_v_ ==
          &new_candidates_q_.front()) {
        candidates_[c.replaced_idx_].candidates_v_ = nullptr;
      }
      candidates_[c.replaced_idx_].feasign_values_ =
          std::move(c.feasign_values_);
    }

    std::lock_guard<std::mutex> lock(mutex4cache_);
    candidates_cache_.push_back(std::move(new_candidates_q_.front()));

    new_candidates_q_.pop_front();
  }

 private:
  size_t capacity_ = 0;
  size_t id_ = 0;
  size_t size_ = 0;

  std::set<uint16_t> replaced_slots_;

  std::vector<FeasignValuesCandidate> candidates_;
  std::deque<std::vector<FeasignValuesCandidate>> new_candidates_q_;
  std::deque<std::vector<FeasignValuesCandidate>> candidates_cache_;

  std::mutex mutex4cache_;
};

struct FeasignValuesReplacer {
  std::vector<std::vector<uint64_t>> feasign_values_;

  void replace(FeasignValues* fea,
               const std::unordered_map<uint16_t, std::vector<uint64_t>>& vals,
               const std::set<uint16_t>& slot_idxs, int* del_num,
               int* add_num) {
    size_t i = 0;
    feasign_values_.resize(slot_idxs.size());
    for (auto idx : slot_idxs) {
      const auto& begin = fea->slot_values.begin() + fea->slot_offsets[idx];
      const auto& end = fea->slot_values.begin() + fea->slot_offsets[idx + 1];

      feasign_values_[i].insert(feasign_values_[i].begin(), begin, end);
      fea->slot_values.erase(begin, end);
      fea->slot_values.insert(begin, vals.at(idx).begin(), vals.at(idx).end());

      // update offset
      int diff = static_cast<int>(vals.at(idx).size()) -
                 static_cast<int>(feasign_values_[i].size());
      for (size_t i = idx + 1; i < fea->slot_offsets.size(); ++i) {
        fea->slot_offsets[i] += diff;
      }

      *del_num += feasign_values_[i].size();
      *add_num += vals.at(idx).size();

      ++i;
    }
  }

  void replace_back(FeasignValues* fea, const std::set<uint16_t>& slot_idxs,
                    int* del_num, int* add_num) {
    size_t i = 0;
    for (auto idx : slot_idxs) {
      const auto& begin = fea->slot_values.begin() + fea->slot_offsets[idx];
      const auto& end = fea->slot_values.begin() + fea->slot_offsets[idx + 1];
      size_t dis = std::distance(begin, end);

      fea->slot_values.erase(begin, end);
      fea->slot_values.insert(begin, feasign_values_[i].begin(),
                              feasign_values_[i].end());

      // update offset
      int diff =
          static_cast<int>(feasign_values_[i].size()) - static_cast<int>(dis);
      for (size_t i = idx + 1; i < fea->slot_offsets.size(); ++i) {
        fea->slot_offsets[i] += diff;
      }

      *del_num += dis;
      *add_num += feasign_values_[i].size();

      ++i;
    }

    feasign_values_.clear();
  }
};

struct AllSlotInfo {
  std::string slot;
  std::string type;
  int used_idx;
  int slot_value_idx;
};

class ISlotParser {
 public:
  virtual ~ISlotParser() {}
  virtual bool Init(const std::vector<AllSlotInfo>& slots) = 0;
  virtual bool ParseOneInstance(
      const std::string& line,
      std::function<int(std::vector<float>&)> GetGpuCacheIndexFunc,  // NOLINT
      std::function<void(std::vector<SlotRecord>&, int)>
          GetInsFunc) {  // NOLINT
    return true;
  }
  virtual bool ParseOneInstance(
      const std::string& line,
      std::function<void(std::vector<SlotRecord>&, int)>
          GetInsFunc) {  // NOLINT
    return true;
  }
  virtual bool ParseOneInstance(
      const std::string& line,
      std::function<uint64_t(std::string&)> GetOffsetFunc,  // NOLINT
      std::function<void(std::vector<SlotRecord>&, int)>
          GetInsFunc) {  // NOLINT
    return true;
  }
  virtual bool ParseIndexData(
      const std::string& line,
      std::function<void(std::string&, std::vector<float>&)>
          AddIndexDataFunc) {  // NOLINT
    return true;
  }
  virtual int UnrollInstance(
      std::vector<SlotRecord>& items, int ins_num,                    // NOLINT
      std::function<void(std::vector<SlotRecord>&)> RealeseMemory) {  // NOLINT
    return 1;
  }
  // user defined file format analysis
  virtual bool ParseFileInstance(
      std::function<int(char* buf, int len)> ReadBuffFunc,
      std::function<void(std::vector<SlotRecord>&, int, int)>
          PullRecordsFunc,  // NOLINT
      int& lines) {         // NOLINT
    return false;
  }
};
struct UsedSlotInfo {
  int idx;
  int slot_value_idx;
  std::string slot;
  std::string type;
  bool dense;
  std::vector<int> local_shape;
  int total_dims_without_inductive;
  int inductive_shape_index;
};
#if defined(PADDLE_WITH_CUDA) && defined(_LINUX)
struct UsedSlotGpuType {
  int is_uint64_value;
  int slot_value_idx;
};
#define CUDA_CHECK(val) CHECK(val == cudaSuccess)
struct BatchCPUValue {
  std::vector<int> h_uint64_lens;
  std::vector<uint64_t> h_uint64_keys;
  std::vector<int> h_uint64_offset;

  std::vector<int> h_float_lens;
  std::vector<float> h_float_keys;
  std::vector<int> h_float_offset;

  std::vector<int> h_rank;
  std::vector<int> h_cmatch;
  std::vector<int> h_ad_offset;
};

struct BatchGPUValue {
  Tensor d_uint64_lens;
  Tensor d_uint64_keys;
  Tensor d_uint64_offset;

  Tensor d_float_lens;
  Tensor d_float_keys;
  Tensor d_float_offset;

  Tensor d_rank;
  Tensor d_cmatch;
  Tensor d_ad_offset;
};

class SlotPaddleBoxDataFeed;
class MiniBatchGpuPack {
 public:
  MiniBatchGpuPack(const paddle::platform::Place& place,
                   const std::vector<UsedSlotInfo>& infos);
  ~MiniBatchGpuPack();
  void reset(const paddle::platform::Place& place);
  void pack_pvinstance(const SlotPvInstance* pv_ins, int num);
  void pack_instance(const SlotRecord* ins_vec, int num);
  int ins_num() { return ins_num_; }
  int pv_num() { return pv_num_; }
  BatchGPUValue& value() { return value_; }
  BatchCPUValue& cpu_value() { return buf_; }
  UsedSlotGpuType* get_gpu_slots(void) {
    return reinterpret_cast<UsedSlotGpuType*>(gpu_slots_->ptr());
  }
  SlotRecord* get_records(void) { return &ins_vec_[0]; }
  double pack_time_span(void) { return pack_timer_.ElapsedSec(); }
  double trans_time_span(void) { return trans_timer_.ElapsedSec(); }

  // tensor gpu memory reused
  void resize_tensor(void) {
    if (used_float_num_ > 0) {
      int float_total_len = buf_.h_float_lens.back() + used_float_num_;
      if (float_total_len > 0) {
        float_tensor_.mutable_data<float>({float_total_len, 1}, this->place_);
      }
    }
    if (used_uint64_num_ > 0) {
      int uint64_total_len = buf_.h_uint64_lens.back() + used_uint64_num_;
      if (uint64_total_len > 0) {
        uint64_tensor_.mutable_data<int64_t>({uint64_total_len, 1},
                                             this->place_);
      }
    }
    //    fprintf(stdout, "float total: %d, uint64: %d\n", float_total_len,
    //          uint64_total_len);
  }
  LoDTensor& float_tensor(void) { return float_tensor_; }
  LoDTensor& uint64_tensor(void) { return uint64_tensor_; }

  std::vector<size_t>& offsets(void) { return offsets_; }
  std::vector<void*>& h_tensor_ptrs(void) { return h_tensor_ptrs_; }

  size_t* gpu_slot_offsets(void) {
    return reinterpret_cast<size_t*>(gpu_slot_offsets_.data<int64_t>());
  }

  void* slot_buf_ptr(void) { return slot_buf_ptr_->ptr(); }

  void resize_gpu_slot_offsets(const int64_t slot_total_num) {
    gpu_slot_offsets_.mutable_data<int64_t>({slot_total_num, 1}, this->place_);
  }
  const std::string& get_lineid(int idx) {
    if (enable_pv_) {
      return ins_vec_[idx]->ins_id_;
    }
    return batch_ins_[idx]->ins_id_;
  }
  // store pcoc q value
  void store_qvalue(const std::vector<Tensor>& qvalue);
  // pack pcoc q to gpu
  void pack_qvalue(void);

 private:
  void transfer_to_gpu(void);
  void pack_all_data(const SlotRecord* ins_vec, int num);
  void pack_uint64_data(const SlotRecord* ins_vec, int num);
  void pack_float_data(const SlotRecord* ins_vec, int num);

 public:
  template <typename T>
  void copy_host2device(Tensor* buf, const T* val, size_t size) {
    if (size == 0) {
      return;
    }
    T* data = buf->mutable_data<T>({static_cast<int64_t>(size), 1}, place_);
    CUDA_CHECK(cudaMemcpyAsync(data, val, size * sizeof(T),
                               cudaMemcpyHostToDevice, stream_));
  }

 private:
  paddle::platform::Place place_;
  cudaStream_t stream_;
  BatchGPUValue value_;
  BatchCPUValue buf_;
  int ins_num_ = 0;
  int pv_num_ = 0;

  bool enable_pv_ = false;
  int used_float_num_ = 0;
  int used_uint64_num_ = 0;
  int used_slot_size_ = 0;

  std::shared_ptr<paddle::memory::allocation::Allocation> gpu_slots_ = nullptr;
  std::vector<UsedSlotGpuType> gpu_used_slots_;
  std::vector<SlotRecord> ins_vec_;
  const SlotRecord* batch_ins_ = nullptr;

  platform::Timer pack_timer_;
  platform::Timer trans_timer_;

  // uint64 tensor
  LoDTensor uint64_tensor_;
  // float tensor
  LoDTensor float_tensor_;
  // batch
  std::vector<size_t> offsets_;
  std::vector<void*> h_tensor_ptrs_;
  // slot offset
  LoDTensor gpu_slot_offsets_;
  std::shared_ptr<paddle::memory::allocation::Allocation> slot_buf_ptr_ =
      nullptr;
  // pcoc
  const int extend_dim_ = FLAGS_padbox_slotrecord_extend_dim;
  LoDTensor* qvalue_tensor_ = nullptr;
};
class MiniBatchGpuPackMgr {
  static const int MAX_DEIVCE_NUM = 16;

 public:
  MiniBatchGpuPackMgr() {
    for (int i = 0; i < MAX_DEIVCE_NUM; ++i) {
      pack_list_[i] = nullptr;
    }
  }
  ~MiniBatchGpuPackMgr() {
    for (int i = 0; i < MAX_DEIVCE_NUM; ++i) {
      if (pack_list_[i] == nullptr) {
        continue;
      }
      delete pack_list_[i];
      pack_list_[i] = nullptr;
    }
  }
  // one device one thread
  MiniBatchGpuPack* get(const paddle::platform::Place& place,
                        const std::vector<UsedSlotInfo>& infos) {
    int device_id = boost::get<platform::CUDAPlace>(place).GetDeviceId();
    if (pack_list_[device_id] == nullptr) {
      pack_list_[device_id] = new MiniBatchGpuPack(place, infos);
    } else {
      pack_list_[device_id]->reset(place);
    }
    return pack_list_[device_id];
  }

  // store pcoc q value
  void store_qvalue(const int device_id, const std::vector<Tensor>& qvalue) {
    pack_list_[device_id]->store_qvalue(qvalue);
  }

 private:
  MiniBatchGpuPack* pack_list_[MAX_DEIVCE_NUM];
};
// global mgr
inline MiniBatchGpuPackMgr& BatchGpuPackMgr() {
  static MiniBatchGpuPackMgr mgr;
  return mgr;
}
#endif
/**
 * @Brief binary archive file
 */
class BinaryArchiveWriter {
 public:
  BinaryArchiveWriter();
  ~BinaryArchiveWriter();
  bool open(const std::string& path);
  bool write(const SlotRecord& rec);
  void close(void);

 private:
  std::mutex mutex_;
  int fd_;
  char* buff_ = nullptr;
  int woffset_ = 0;
  int capacity_ = 0;
  char* head_ = nullptr;
};
class SlotPaddleBoxDataFeed : public DataFeed {
 public:
  SlotPaddleBoxDataFeed() { finish_start_ = false; }
  virtual ~SlotPaddleBoxDataFeed() {
#if defined(PADDLE_WITH_CUDA) && defined(_LINUX)
    if (pack_ != nullptr) {
      LOG(WARNING) << "gpu: "
                   << boost::get<platform::CUDAPlace>(place_).GetDeviceId()
                   << ", pack batch total time: " << batch_timer_.ElapsedSec()
                   << "[copy:" << pack_->trans_time_span()
                   << ",fill:" << fill_timer_.ElapsedSec()
                   << ",memory:" << offset_timer_.ElapsedSec()
                   << ",offset:" << copy_timer_.ElapsedSec()
                   << ",tensor:" << data_timer_.ElapsedSec()
                   << ",trans:" << trans_timer_.ElapsedSec()
                   << "], batch cpu build mem: " << pack_->pack_time_span()
                   << "sec";
      pack_ = nullptr;
    }
#endif
  }

  // This function will do nothing at default
  virtual void SetInputChannel(void* channel) {
    input_channel_ = static_cast<ChannelObject<SlotRecord>*>(channel);
  }
  // This function will do nothing at default
  virtual void SetThreadId(int thread_id) { thread_id_ = thread_id; }
  // This function will do nothing at default
  virtual void SetThreadNum(int thread_num) { thread_num_ = thread_num; }
  // This function will do nothing at default
  virtual void SetParseInsId(bool parse_ins_id) {
    parse_ins_id_ = parse_ins_id;
  }
  virtual void SetParseLogKey(bool parse_logkey) {
    parse_logkey_ = parse_logkey;
  }
  virtual void SetEnablePvMerge(bool enable_pv_merge) {
    enable_pv_merge_ = enable_pv_merge;
  }
  virtual void SetCurrentPhase(int current_phase) {
    current_phase_ = current_phase;
  }
  virtual const std::string& GetLineId(int idx) const {
    return pack_->get_lineid(idx);
  }
  virtual int GetCurBatchSize() { return pack_->ins_num(); }

 public:
  int GetBatchSize() { return default_batch_size_; }
  int GetPvBatchSize() { return pv_batch_size_; }
  void SetPvInstance(SlotPvInstance* pv_ins) { pv_ins_ = pv_ins; }
  void SetSlotRecord(SlotRecord* records) { records_ = records; }
  void AddBatchOffset(const std::pair<int, int>& off) {
    batch_offsets_.push_back(off);
  }
  void GetUsedSlotIndex(std::vector<int>* used_slot_index);
  // expand values
  void ExpandSlotRecord(SlotRecord* ins);
  // pack
  bool EnablePvMerge(void);
  int GetPackInstance(SlotRecord** ins);
  int GetPackPvInstance(SlotPvInstance** pv_ins);
  void SetSlotRecordPool(SlotObjPool* pool) { slot_pool_ = pool; }

 public:
  virtual void Init(const DataFeedDesc& data_feed_desc);
  virtual bool Start();
  virtual int Next();
  virtual void AssignFeedVar(const Scope& scope);
  virtual int GetCurrentPhase();
  virtual void LoadIntoMemory();
  virtual void UnrollInstance(std::vector<SlotRecord>& items);  // NOLINT

 protected:
  virtual void LoadIntoMemoryByCommand(void);
  virtual void LoadIntoMemoryByLib(void);
  void PutToFeedPvVec(const SlotPvInstance* pvs, int num);
  void PutToFeedSlotVec(const SlotRecord* recs, int num);
  void BuildSlotBatchGPU(const int ins_num);
  void GetRankOffsetGPU(const int pv_num, const int ins_num);
  void GetRankOffset(const SlotPvInstance* pv_vec, int pv_num, int ins_number);
  bool ParseOneInstance(const std::string& line, SlotRecord* rec);

 protected:
  // \n split by line
  virtual void LoadIntoMemoryByLine(void);
  // split all file
  virtual void LoadIntoMemoryByFile(void);
  // load local archive file
  virtual void LoadIntoMemoryByArchive(void);

 private:
#if defined(PADDLE_WITH_CUDA) && defined(_LINUX)
  void CopyRankOffset(int* dest, const int ins_num, const int pv_num,
                      const int max_rank, const int* ranks, const int* cmatchs,
                      const int* ad_offsets, const int cols);
  void FillSlotValueOffset(const int ins_num, const int used_slot_num,
                           size_t* slot_value_offsets,
                           const int* uint64_offsets,
                           const int uint64_slot_size, const int* float_offsets,
                           const int float_slot_size,
                           const UsedSlotGpuType* used_slots);
  void CopyForTensor(const int ins_num, const int used_slot_num, void** dest,
                     const size_t* slot_value_offsets,
                     const uint64_t* uint64_feas, const int* uint64_offsets,
                     const int* uint64_ins_lens, const int uint64_slot_size,
                     const float* float_feas, const int* float_offsets,
                     const int* float_ins_lens, const int float_slot_size,
                     const UsedSlotGpuType* used_slots);
#endif

 protected:
  int thread_id_ = 0;
  int thread_num_ = 0;
  bool parse_ins_id_ = false;
  bool parse_content_ = false;
  bool parse_logkey_ = false;
  bool enable_pv_merge_ = false;
  int current_phase_{-1};  // only for untest
  std::shared_ptr<FILE> fp_ = nullptr;
  ChannelObject<SlotRecord>* input_channel_ = nullptr;

  std::vector<std::vector<float>> batch_float_feasigns_;
  std::vector<std::vector<uint64_t>> batch_uint64_feasigns_;
  std::vector<std::vector<size_t>> offset_;
  std::vector<int> float_total_dims_without_inductives_;
  size_t float_total_dims_size_ = 0;

  std::string rank_offset_name_;
  int pv_batch_size_ = 0;
  int use_slot_size_ = 0;
  int float_use_slot_size_ = 0;
  int uint64_use_slot_size_ = 0;

#if defined(PADDLE_WITH_CUDA) && defined(_LINUX)
  MiniBatchGpuPack* pack_ = nullptr;
#endif
  int offset_index_ = 0;
  std::vector<std::pair<int, int>> batch_offsets_;
  SlotPvInstance* pv_ins_ = nullptr;
  SlotRecord* records_ = nullptr;
  std::vector<AllSlotInfo> all_slots_info_;
  std::vector<UsedSlotInfo> used_slots_info_;
  std::string parser_so_path_;

  platform::Timer batch_timer_;
  platform::Timer fill_timer_;
  platform::Timer offset_timer_;
  platform::Timer data_timer_;
  platform::Timer trans_timer_;
  platform::Timer copy_timer_;
  SlotObjPool* slot_pool_ = nullptr;
};

class SlotPaddleBoxDataFeedWithGpuReplicaCache : public SlotPaddleBoxDataFeed {
 public:
  SlotPaddleBoxDataFeedWithGpuReplicaCache() { finish_start_ = false; }

 private:
  virtual void LoadIntoMemoryByLib(void);
  virtual void LoadIntoMemoryByCommand(void);
  bool ParseOneInstance(const std::string& line, SlotRecord* rec,
                        int gpu_cache_offset);
};

class InputTableDataFeed : public SlotPaddleBoxDataFeed {
 protected:
  virtual void LoadIntoMemoryByCommand(void) {
    PADDLE_THROW(
        "InputTableDataFeed is not implemented LoadIntoMemoryByCommand");
  }
  virtual void LoadIntoMemoryByLib(void);
};

class InputIndexDataFeed : public DataFeed {
 public:
  void Init(const DataFeedDesc& data_feed_desc) override {
    pipe_command_ = data_feed_desc.index_parser();
    parser_so_path_ = paddle::string::erase_spaces(pipe_command_);
    VLOG(3) << "InputIndexDataFeed parser: " << parser_so_path_;

    size_t pos = pipe_command_.find(".so");
    CHECK(pos != std::string::npos);
    pipe_command_.clear();

    finish_init_ = true;
  }
  bool Start() override { return true; }
  int Next() override { return 0; }
  void SetThreadId(int thread_id) { thread_id_ = thread_id; }
  void LoadIntoMemory() override;

 protected:
  int thread_id_ = 0;
  std::string parser_so_path_;
  std::shared_ptr<FILE> fp_ = nullptr;
};

template <class AR, class T>
paddle::framework::Archive<AR>& operator<<(paddle::framework::Archive<AR>& ar,
                                           const SlotValues<T>& r) {
  uint32_t value_len = static_cast<uint32_t>(r.slot_values.size());
  ar << value_len;
  if (value_len > 0) {
    ar.Write(&r.slot_values[0], value_len * sizeof(T));

    uint16_t slot_num = static_cast<uint16_t>(r.slot_offsets.size());
    ar << slot_num;
    if (slot_num > 2) {
      // remove first 0 and end data len
      ar.Write(&r.slot_offsets[1], (slot_num - 2) * sizeof(uint32_t));
    }
  }
  return ar;
}
template <class AR, class T>
paddle::framework::Archive<AR>& operator>>(paddle::framework::Archive<AR>& ar,
                                           SlotValues<T>& r) {
  uint32_t value_len = 0;
  ar >> value_len;
  if (value_len > 0) {
    r.slot_values.resize(value_len);
    ar.Read(&r.slot_values[0], value_len * sizeof(T));

    uint16_t slot_num = 0;
    ar >> slot_num;
    r.slot_offsets.resize(slot_num);
    // fill first 0
    r.slot_offsets[0] = 0;
    if (slot_num > 2) {
      ar.Read(&r.slot_offsets[1], (slot_num - 2) * sizeof(uint32_t));
    }
    // fill end data len
    r.slot_offsets[slot_num - 1] = value_len;
  }
  return ar;
}
template <class AR>
paddle::framework::Archive<AR>& operator<<(paddle::framework::Archive<AR>& ar,
                                           const SlotRecord& r) {
  ar << r->slot_float_feasigns_;
  ar << r->slot_uint64_feasigns_;
  ar << r->ins_id_;
  ar << r->search_id;
  ar << r->rank;
  ar << r->cmatch;

  return ar;
}
template <class AR>
paddle::framework::Archive<AR>& operator>>(paddle::framework::Archive<AR>& ar,
                                           SlotRecord& r) {
  ar >> r->slot_float_feasigns_;
  ar >> r->slot_uint64_feasigns_;
  ar >> r->ins_id_;
  ar >> r->search_id;
  ar >> r->rank;
  ar >> r->cmatch;

  return ar;
}
#endif

#if defined(PADDLE_WITH_CUDA) && !defined(_WIN32)
template <typename T>
class PrivateInstantDataFeed : public DataFeed {
 public:
  PrivateInstantDataFeed() {}
  virtual ~PrivateInstantDataFeed() {}
  void Init(const DataFeedDesc& data_feed_desc) override;
  bool Start() override { return true; }
  int Next() override;

 protected:
  // The batched data buffer
  std::vector<MultiSlotType> ins_vec_;

  // This function is used to preprocess with a given filename, e.g. open it or
  // mmap
  virtual bool Preprocess(const std::string& filename) = 0;

  // This function is used to postprocess system resource such as closing file
  // NOTICE: Ensure that it is safe to call before Preprocess
  virtual bool Postprocess() = 0;

  // The reading and parsing method.
  virtual bool ParseOneMiniBatch() = 0;

  // This function is used to put ins_vec to feed_vec
  virtual void PutToFeedVec();
};

class MultiSlotFileInstantDataFeed
    : public PrivateInstantDataFeed<std::vector<MultiSlotType>> {
 public:
  MultiSlotFileInstantDataFeed() {}
  virtual ~MultiSlotFileInstantDataFeed() {}

 protected:
  int fd_{-1};
  char* buffer_{nullptr};
  size_t end_{0};
  size_t offset_{0};

  bool Preprocess(const std::string& filename) override;

  bool Postprocess() override;

  bool ParseOneMiniBatch() override;
};
#endif

}  // namespace framework
}  // namespace paddle
