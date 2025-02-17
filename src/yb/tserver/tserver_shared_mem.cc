// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/tserver/tserver_shared_mem.h"

#include <atomic>
#include <mutex>

#include <boost/interprocess/shared_memory_object.hpp>

#include "yb/gutil/casts.h"

#include "yb/util/enums.h"
#include "yb/util/env.h"
#include "yb/util/flags.h"
#include "yb/util/path_util.h"
#include "yb/util/result.h"
#include "yb/util/thread.h"

DEFINE_test_flag(bool, skip_remove_tserver_shared_memory_object, false,
                 "Skip remove tserver shared memory object in tests.");

namespace yb::tserver {

namespace {

using SystemClock = std::chrono::system_clock;

std::chrono::system_clock::time_point ToSystemBase() {
  auto now_system = SystemClock::now();
  auto coarse_since_epoch = CoarseMonoClock::now().time_since_epoch();
  return now_system - std::chrono::duration_cast<SystemClock::duration>(coarse_since_epoch);
}

std::chrono::system_clock::time_point ToSystem(CoarseTimePoint tp) {
  static SystemClock::time_point base = ToSystemBase();
  if (tp == CoarseTimePoint()) {
    return SystemClock::time_point::max();
  }
  return base + std::chrono::duration_cast<SystemClock::duration>(tp.time_since_epoch());
}

YB_DEFINE_ENUM(SharedExchangeState,
               (kIdle)(kRequestSent)(kResponseSent)(kShutdown));

class SharedExchangeHeader {
 public:
  SharedExchangeHeader() = default;

  std::byte* data() {
    return data_;
  }

  size_t header_size() {
    return data() - pointer_cast<std::byte*>(this);
  }

  bool ReadyToSend(bool failed_previous_request) const {
    return ReadyToSend(state_.load(std::memory_order_acquire), failed_previous_request);
  }

  bool ReadyToSend(SharedExchangeState state, bool failed_previous_request) const {
    // Could use this exchange for sending request in two cases:
    // 1) it is idle, i.e. no request is being processed at this moment.
    // 2) the previous request was failed, and we received response for this request.
    return state == SharedExchangeState::kIdle ||
           (failed_previous_request && state == SharedExchangeState::kResponseSent);
  }

  Result<size_t> SendRequest(
      bool failed_previous_request, uint64_t session_id,
      size_t size, std::chrono::system_clock::time_point deadline) {
    std::unique_lock<boost::interprocess::interprocess_mutex> lock(mutex_);
    auto state = state_.load(std::memory_order_acquire);
    if (!ReadyToSend(failed_previous_request)) {
      lock.unlock();
      return STATUS_FORMAT(IllegalState, "Send request in wrong state: $0", state);
    }
    state_.store(SharedExchangeState::kRequestSent, std::memory_order_release);
    data_size_ = size;
    cond_.notify_one();

    RETURN_NOT_OK(DoWait(SharedExchangeState::kResponseSent, deadline, &lock));
    state_.store(SharedExchangeState::kIdle, std::memory_order_release);
    return data_size_;
  }

  void Respond(size_t size) {
    std::unique_lock<boost::interprocess::interprocess_mutex> lock(mutex_);
    auto state = state_.load(std::memory_order_acquire);
    if (state != SharedExchangeState::kRequestSent) {
      lock.unlock();
      LOG_IF(DFATAL, state != SharedExchangeState::kShutdown)
          << "Respond in wrong state: " << AsString(state);
      return;
    }

    data_size_ = size;
    state_.store(SharedExchangeState::kResponseSent, std::memory_order_release);
    cond_.notify_one();
  }

  Result<size_t> Poll() {
    std::unique_lock<boost::interprocess::interprocess_mutex> lock(mutex_);
    RETURN_NOT_OK(DoWait(
        SharedExchangeState::kRequestSent, std::chrono::system_clock::time_point::max(), &lock));
    return data_size_;
  }

  void SignalStop() {
    std::unique_lock<boost::interprocess::interprocess_mutex> lock(mutex_);
    state_.store(SharedExchangeState::kShutdown, std::memory_order_release);
    cond_.notify_all();
  }

 private:
  Status DoWait(SharedExchangeState expected_state, std::chrono::system_clock::time_point deadline,
                std::unique_lock<boost::interprocess::interprocess_mutex>* lock) {
    for (;;) {
      auto state = state_.load(std::memory_order_acquire);
      if (state == expected_state) {
        return Status::OK();
      }
      if (state == SharedExchangeState::kShutdown) {
        lock->unlock();
        return STATUS_FORMAT(ShutdownInProgress, "Shutting down shared exchange");
      }
      if (!cond_.timed_wait(*lock, deadline)) {
        state = state_.load(std::memory_order_acquire);
        lock->unlock();
        return STATUS_FORMAT(TimedOut, "Timed out waiting $0, state: $1", expected_state, state);
      }
    }
  }

  boost::interprocess::interprocess_mutex mutex_;
  boost::interprocess::interprocess_condition cond_;
  std::atomic<SharedExchangeState> state_{SharedExchangeState::kIdle};
  size_t data_size_;
  std::byte data_[0];
};

std::string MakeSharedMemoryPrefix(const std::string& instance_id) {
  return Format("yb_pg_$0_", instance_id);
}

std::string MakeSharedMemoryName(const std::string& instance_id, uint64_t session_id) {
  return MakeSharedMemoryPrefix(instance_id) + std::to_string(session_id);
}

} // namespace

class SharedExchange::Impl {
 public:
  template <class T>
  Impl(T type, const std::string& instance_id, uint64_t session_id)
      : session_id_(session_id),
        owner_(std::is_same_v<T, boost::interprocess::create_only_t>),
        shared_memory_object_(type, MakeSharedMemoryName(instance_id, session_id).c_str(),
                              boost::interprocess::read_write) {
    if (owner_) {
      shared_memory_object_.truncate(boost::interprocess::mapped_region::get_page_size());
    }
    mapped_region_ = boost::interprocess::mapped_region(
        shared_memory_object_, boost::interprocess::read_write);
    if (owner_) {
      new (mapped_region_.get_address()) SharedExchangeHeader();
    }
  }

  ~Impl() {
    if (!owner_ || FLAGS_TEST_skip_remove_tserver_shared_memory_object) {
      return;
    }
    std::string shared_memory_object_name(shared_memory_object_.get_name());
    shared_memory_object_ = boost::interprocess::shared_memory_object();
    boost::interprocess::shared_memory_object::remove(shared_memory_object_name.c_str());
  }

  std::byte* Obtain(size_t required_size) {
    last_size_ = required_size;
    auto* header = &this->header();
    required_size += header->header_size();
    auto region_size = mapped_region_.get_size();
    if (required_size > region_size) {
      return nullptr;
    }
    return header->data();
  }

  uint64_t session_id() const {
    return session_id_;
  }

  Result<Slice> SendRequest(CoarseTimePoint deadline) {
    auto* header = &this->header();
    auto size_res = header->SendRequest(
        failed_previous_request_, session_id_, last_size_, ToSystem(deadline));
    if (!size_res.ok()) {
      failed_previous_request_ = true;
      return size_res.status();
    }
    failed_previous_request_ = false;
    if (*size_res + header->header_size() > mapped_region_.get_size()) {
      return Slice(static_cast<const char*>(nullptr), bit_cast<const char*>(*size_res));
    }
    return Slice(header->data(), *size_res);
  }

  bool ReadyToSend() const {
    return header().ReadyToSend(failed_previous_request_);
  }

  void Respond(size_t size) {
    header().Respond(size);
  }

  Result<size_t> Poll() {
    return header().Poll();
  }

  void SignalStop() {
    header().SignalStop();
  }

 private:
  SharedExchangeHeader& header() {
    return *static_cast<SharedExchangeHeader*>(mapped_region_.get_address());
  }

  const SharedExchangeHeader& header() const {
    return *static_cast<SharedExchangeHeader*>(mapped_region_.get_address());
  }

  const uint64_t session_id_;
  const bool owner_;
  boost::interprocess::shared_memory_object shared_memory_object_;
  boost::interprocess::mapped_region mapped_region_;
  size_t last_size_;
  bool failed_previous_request_ = false;
};

SharedExchange::SharedExchange(const std::string& instance_id, uint64_t session_id, Create create) {
  try {
    if (create) {
      impl_ = std::make_unique<Impl>(boost::interprocess::create_only, instance_id, session_id);
    } else {
      impl_ = std::make_unique<Impl>(boost::interprocess::open_only, instance_id, session_id);
    }
  } catch (boost::interprocess::interprocess_exception& exc) {
    LOG(FATAL) << "Failed to create shared exchange for " << instance_id << "/" << session_id
               << ", mode: " << create << ", error: " << exc.what();
  }
}

SharedExchange::~SharedExchange() = default;

Status SharedExchange::Cleanup(const std::string& instance_id) {
  std::string dir;
#if defined(BOOST_INTERPROCESS_POSIX_SHARED_MEMORY_OBJECTS)
  dir = "/dev/shm";
#else
  boost::interprocess::ipcdetail::get_shared_dir(dir);
#endif
  auto& env = *Env::Default();
  auto files = VERIFY_RESULT(env.GetChildren(dir, ExcludeDots::kTrue));
  auto prefix = MakeSharedMemoryPrefix(instance_id);
  for (const auto& file : files) {
    if (boost::starts_with(file, prefix)) {
      boost::interprocess::shared_memory_object::remove(file.c_str());
    }
  }
  return Status::OK();
}

std::byte* SharedExchange::Obtain(size_t required_size) {
  return impl_->Obtain(required_size);
}

Result<Slice> SharedExchange::SendRequest(CoarseTimePoint deadline) {
  return impl_->SendRequest(deadline);
}

bool SharedExchange::ReadyToSend() const {
  return impl_->ReadyToSend();
}

void SharedExchange::Respond(size_t size) {
  return impl_->Respond(size);
}

Result<size_t> SharedExchange::Poll() {
  return impl_->Poll();
}

void SharedExchange::SignalStop() {
  impl_->SignalStop();
}

uint64_t SharedExchange::session_id() const {
  return impl_->session_id();
}

SharedExchangeThread::SharedExchangeThread(
    const std::string& instance_id, uint64_t session_id, Create create,
    const SharedExchangeListener& listener)
    : exchange_(instance_id, session_id, create) {
  CHECK_OK(Thread::Create(
      "shared_exchange", Format("sh_xchng_$0", session_id), [this, listener] {
    CDSAttacher cdc_attacher;
    for (;;) {
      auto query_size = exchange_.Poll();
      if (!query_size.ok()) {
        if (!query_size.status().IsShutdownInProgress()) {
          LOG(DFATAL) << "Poll session " << exchange_.session_id() <<  " failed: "
                      << query_size.status();
        }
        break;
      }
      listener(*query_size);
    }
  }, &thread_));
}

SharedExchangeThread::~SharedExchangeThread() {
  exchange_.SignalStop();
  thread_->Join();
}

} // namespace yb::tserver
