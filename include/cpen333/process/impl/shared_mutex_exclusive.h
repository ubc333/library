#ifndef CPEN333_PROCESS_SHARED_MUTEX_EXCLUSIVE_H
#define CPEN333_PROCESS_SHARED_MUTEX_EXCLUSIVE_H

#define SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX "__shared_mutex_exclusive__"
#define SHARED_MUTEX_EXCLUSIVE_MUTEX_SUFFIX "__shared_mutex_m_exclusive__"
#define SHARED_MUTEX_EXCLUSIVE_INITIALIZED 0x98292338

#include "cpen333/process/mutex.h"
#include "cpen333/process/semaphore.h"
#include "cpen333/process/condition.h"
#include "cpen333/process/shared_memory.h"

namespace cpen333 {
namespace process {

namespace impl {

// Write-preferring
/**
 * Shared-mutex implementation based on the mutex/semaphore pattern
 * See https://en.wikipedia.org/wiki/Readers%E2%80%93writer_lock for details
 */
class shared_mutex_exclusive : named_resource {
 protected:

  struct shared_data {
    size_t shared;
    size_t exclusive;
    int initialized;
  };

  cpen333::process::mutex shared_;                        // mutex for shared access
  cpen333::process::semaphore global_;                    // global semaphore
  cpen333::process::shared_object<shared_data> count_;    // shared counter object
  cpen333::process::mutex exclusive_;                     // protect writes to the "exclusive" field
  cpen333::process::condition cond_;                      // condition of no writers

 public:
  shared_mutex_exclusive(const std::string &name) :
      named_resource{name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX)},
      shared_{name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX)},
      global_{name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX), 1},   // gate opened
      count_{name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX)},
      exclusive_{name + std::string(SHARED_MUTEX_EXCLUSIVE_MUTEX_SUFFIX)},
      cond_{name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX), true}  // gate opened
  {

    // initialize storage
    std::lock_guard<decltype(shared_)> lock(shared_);
    if (count_->initialized != SHARED_MUTEX_EXCLUSIVE_INITIALIZED) {
      count_->shared = 0;
      count_->exclusive = 0;
      count_->initialized = SHARED_MUTEX_EXCLUSIVE_INITIALIZED;
    }
  }

  // disable copy/move constructors
  shared_mutex_exclusive(const shared_mutex_exclusive &) = delete;
  shared_mutex_exclusive(shared_mutex_exclusive &&) = delete;
  shared_mutex_exclusive &operator=(const shared_mutex_exclusive &) = delete;
  shared_mutex_exclusive &operator=(shared_mutex_exclusive &&) = delete;

  void lock_shared() {

    cond_.wait();           // wait until no exclusive access

    // increment number of waiting readers
    std::lock_guard<cpen333::process::mutex> lock(shared_);
    if (++(count_->shared) == 1) {
      global_.wait();       // "lock" semaphore preventing write access, could be waiting here for writer to finish
    }
  }

  bool try_lock_shared() {
    if (!cond_.wait_for(std::chrono::milliseconds(0))) {
      return false;
    }

    std::unique_lock<cpen333::process::mutex> lock(shared_, std::defer_lock);
    if (!lock.try_lock()) {
      return false;
    }

    if (count_->shared == 0) {
      bool success = global_.try_wait();  // "lock" semaphore preventing writes
      // only increment if successful
      if (!success) {
        return false;
      }
      count_->shared = 1;
    } else {
      ++(count_->shared);
    }
    return true;
  }

  void unlock_shared() {
    std::lock_guard<cpen333::process::mutex> lock(shared_);
    if (--(count_->shared) == 0) {
      global_.notify(); // "unlock" semaphore
    }
  }

  void lock() {
    // next in line
    {
      std::lock_guard<cpen333::process::mutex> lock(shared_);
      // if only one writer, block all future readers
      std::lock_guard<cpen333::process::mutex> ex(exclusive_);
      if (++(count_->exclusive) == 1) {
        cond_.reset();
      }
    }
    global_.wait(); // lock semaphore
  }

  bool try_lock() {

    // next in line
    {
      std::unique_lock<cpen333::process::mutex> lock(shared_, std::defer_lock);
      if (!lock.try_lock()) {
        return false; // someone is currently locked
      }

      // try holding both locks at once immediately
      if (!global_.try_wait()) {
        return false;
      }

      // I now hold both locks, which means there are no current readers or writers
      std::lock_guard<cpen333::process::mutex> ex(exclusive_);
      count_->exclusive = 1;
      cond_.reset();   // prevent future readers
    }

    return true;
  }

  void unlock() {

    global_.notify(); // unlock semaphore  // allow next reader/writer waiting

    std::lock_guard<cpen333::process::mutex> lock(shared_);   // access to data
    std::lock_guard<cpen333::process::mutex> ex(exclusive_);
    if (--(count_->exclusive) == 0) {
      cond_.notify();  // open gate for readers
    }
  }

  /**
   * tries to lock the mutex, returns if the mutex has been unavailable for the specified timeout duration
   * @tparam Rep duration representation
   * @tparam Period duration period
   * @param timeout_duration timeout
   * @return true if locked successfully
   */
  template<class Rep, class Period>
  bool try_lock_for(const std::chrono::duration<Rep, Period> &timeout_duration) {
    return try_lock_until(std::chrono::steady_clock::now() + timeout_duration);
  };

  /**
   * tries to lock the mutex, returns if the mutex has been unavailable until specified time point has been reached
   * @tparam Clock clock representation
   * @tparam Duration time
   * @param timeout_time time of timeout
   * @return true if locked successfully
   */
  template<class Clock, class Duration>
  bool try_lock_until(const std::chrono::time_point<Clock, Duration> &timeout_time) {

    // next in line
    {
      std::unique_lock<cpen333::process::mutex> lock(shared_, std::defer_lock);
      if (!lock.try_lock_until(timeout_time)) {
        return false;
      }

      // notify the line that there is an exclusive lock waiting
      std::lock_guard<cpen333::process::mutex> ex(exclusive_);  // fast lock, doesn't protect any waits
      if (++(count_->exclusive) == 1) {
        cond_.reset();
      }
    }

    if (!global_.wait_until(timeout_time)) {
      // quickly remove exclusive waiter, allowing others to proceed
      std::lock_guard<cpen333::process::mutex> ex(exclusive_);
      if (--(count_->exclusive) == 0) {
        cond_.notify();  // open gate for readers
      }
    }
    return true;
  };

  /**
   * tries to lock the mutex, returns if the mutex has been unavailable for the specified timeout duration
   * @tparam Rep duration representation
   * @tparam Period duration period
   * @param timeout_duration timeout
   * @return true if locked successfully
   */
  template<class Rep, class Period>
  bool try_lock_shared_for(const std::chrono::duration<Rep, Period> &timeout_duration) {
    return try_lock_shared_until(std::chrono::steady_clock::now() + timeout_duration);
  };

  /**
   * tries to lock the mutex, returns if the mutex has been unavailable until specified time point has been reached
   * @tparam Clock clock representation
   * @tparam Duration time
   * @param timeout_time time of timeout
   * @return true if locked successfully
   */
  template<class Clock, class Duration>
  bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration> &timeout_time) {

    if (!cond_.wait_until(timeout_time)) {
      return false;
    }
    // no current exclusives waiting

    std::unique_lock<cpen333::process::mutex> lock(shared_, std::defer_lock);
    if (!lock.try_lock_until(timeout_time)) {
      return false;
    }

    if (count_->shared == 0) {
      bool success = global_.wait_until(timeout_time);  // "lock" semaphore preventing writes
      // only increment if successful
      if (!success) {
        return false;
      }
      count_->shared = 1;
    } else {
      ++(count_->shared);
    }
    return true;
  };

  bool unlink() {
    bool b1 = shared_.unlink();
    bool b2 = global_.unlink();
    bool b3 = count_.unlink();
    bool b4 = cond_.unlink();
    bool b5 = exclusive_.unlink();
    return (b1 && b2 && b3 && b4 && b5);
  }

  static bool unlink(const std::string& name) {
    bool b1 = cpen333::process::mutex::unlink(name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX));
    bool b2 = cpen333::process::semaphore::unlink(name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX));
    bool b3 = cpen333::process::shared_object<shared_data>::unlink(name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX));
    bool b4 = cpen333::process::mutex::unlink(name + std::string(SHARED_MUTEX_EXCLUSIVE_MUTEX_SUFFIX));
    bool b5 = cpen333::process::condition::unlink(name + std::string(SHARED_MUTEX_EXCLUSIVE_NAME_SUFFIX));
    return (b1 && b2 && b3 && b4 && b5);
  }
};

} // impl

using shared_mutex_exclusive = impl::shared_mutex_exclusive;
using shared_timed_mutex_exclusive = impl::shared_mutex_exclusive;

} // process
} // cpen333

#endif //CPEN333_PROCESS_SHARED_MUTEX_EXCLUSIVE_H
