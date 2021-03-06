/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <chrono>

#include <osquery/dispatcher.h>
#include <osquery/flags.h>
#include <osquery/logger.h>

#include "osquery/core/conversions.h"

#if 0
#ifdef DLOG
#undef DLOG
#define DLOG(v) LOG(v)
#endif
#endif

namespace osquery {

/// The worker_threads define the default thread pool size.
FLAG(int32, worker_threads, 4, "Number of work dispatch threads");

/// Cancel the pause request.
void RunnerInterruptPoint::cancel() {
  WriteLock lock(mutex_);
  stop_ = true;
  condition_.notify_all();
}

/// Pause until the requested millisecond delay has elapsed or a cancel.
void RunnerInterruptPoint::pause(std::chrono::milliseconds milli) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stop_ || condition_.wait_for(lock, milli) == std::cv_status::no_timeout) {
    stop_ = false;
    throw RunnerInterruptError();
  }
}

void InterruptableRunnable::interrupt() {
  WriteLock lock(stopping_);
  // Set the service as interrupted.
  interrupted_ = true;
  // Tear down the service's resources such that exiting the expected run
  // loop within ::start does not need to.
  stop();
  // Cancel the run loop's pause request.
  point_.cancel();
}

bool InterruptableRunnable::interrupted() {
  WriteLock lock(stopping_);
  return interrupted_;
}

void InterruptableRunnable::pauseMilli(std::chrono::milliseconds milli) {
  try {
    point_.pause(milli);
  } catch (const RunnerInterruptError&) {
    // The pause request was canceled.
  }
}

Status Dispatcher::addService(InternalRunnableRef service) {
  if (service->hasRun()) {
    return Status(1, "Cannot schedule a service twice");
  }

  auto& self = instance();
  if (self.stopping_) {
    // Cannot add a service while the dispatcher is stopping and no joins
    // have been requested.
    return Status(1, "Cannot add service, dispatcher is stopping");
  }

  auto thread = std::make_shared<std::thread>(
      std::bind(&InternalRunnable::run, &*service));
  WriteLock lock(self.mutex_);
  DLOG(INFO) << "Adding new service: " << &*service
             << " to thread: " << &*thread;
  self.service_threads_.push_back(thread);
  self.services_.push_back(std::move(service));
  return Status(0, "OK");
}

void Dispatcher::joinServices() {
  auto& self = instance();
  DLOG(INFO) << "Thread: " << std::this_thread::get_id()
             << " requesting a join";
  WriteLock join_lock(self.join_mutex_);
  for (auto& thread : self.service_threads_) {
    // Boost threads would have been interrupted, and joined using the
    // provided thread instance.
    thread->join();
    DLOG(INFO) << "Service thread: " << &*thread << " has joined";
  }

  WriteLock lock(self.mutex_);
  self.services_.clear();
  self.service_threads_.clear();
  self.stopping_ = false;
  DLOG(INFO) << "Services and threads have been cleared";
}

void Dispatcher::stopServices() {
  auto& self = instance();
  self.stopping_ = true;

  WriteLock lock(self.mutex_);
  DLOG(INFO) << "Thread: " << std::this_thread::get_id()
             << " requesting a stop";
  for (const auto& service : self.services_) {
    while (true) {
      // Wait for each thread's entry point (start) meaning the thread context
      // was allocated and (run) was called by std::thread started.
      if (service->hasRun()) {
        break;
      }
      // We only need to check if std::terminate is called very quickly after
      // the std::thread is created.
      ::usleep(20);
    }
    service->interrupt();
    DLOG(INFO) << "Service: " << &*service << " has been interrupted";
  }
}
}
