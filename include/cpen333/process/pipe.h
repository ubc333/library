#ifndef CPEN333_PROCESS_PIPE_H
#define CPEN333_PROCESS_PIPE_H

#define PIPE_NAME_SUFFIX "_pp"
#define PIPE_WRITE_SUFFIX "_ppw"
#define PIPE_READ_SUFFIX "_ppr"
#define PIPE_INFO_SUFFIX "_ppi"
#define PIPE_OPEN_SUFFIX "_ppo"
#define PIPE_INITIALIZED 0x18763023

#include "cpen333/process/impl/named_resource.h"
#include "cpen333/process/mutex.h"
#include "cpen333/process/semaphore.h"
#include "cpen333/process/shared_memory.h"

// simulated pipe using shared memory and semaphores
namespace cpen333 {
namespace process {


class pipe : public named_resource {

 public:

  enum mode {
    READ, WRITE
  };

  pipe(const std::string& name, mode mode, size_t size = 1024) :
      named_resource{name + std::string(PIPE_NAME_SUFFIX)},
      mode_{mode},
      wmutex_{name + std::string(PIPE_WRITE_SUFFIX)},
      rmutex_{name + std::string(PIPE_READ_SUFFIX)},
      omutex_{name + std::string(PIPE_OPEN_SUFFIX)},
      info_{name + std::string(PIPE_INFO_SUFFIX)},
      pipe_{name + std::string(PIPE_NAME_SUFFIX), size},
      producer_{name + std::string(PIPE_WRITE_SUFFIX), 0},
      consumer_{name + std::string(PIPE_READ_SUFFIX), size} {

    // potentially initialize info
    std::lock_guard<decltype(wmutex_)> lock(omutex_);
    if (info_->initialized != PIPE_INITIALIZED) {
      info_->size = size;
      info_->read = 0;
      info_->write = 0;
      info_->reof = 0;            // marks 1 past the final written index
      info_->weof = 0;            // marks 1 past the final read index
      info_->has_reader = false;
      info_->has_writer = false;
      info_->initialized = PIPE_INITIALIZED; // mark as initialized
    }

    // check mode and assign
    switch(mode_) {
      case READ: {
        // use write lock for reader to prevent interference
        if (info_->has_reader) {
          cpen333::perror(std::string("Pipe ") + name + std::string(" already has a reader."));
        } else {
          info_->has_reader = true;
        }
        break;
      }
      case WRITE: {
        // use read lock for writer to prevent interference
        if (info_->has_writer) {
          cpen333::perror(std::string("Pipe ") + name + std::string(" already has a writer."));
        } else {
          info_->has_writer = true;
        }
        break;
      }
    }
  }

  ~pipe() {
    close();  // close pipe
  }

  /**
   * Writes data to the pipe
   * @param data data to write
   * @param size size in bytes
   * @return true if pipe is open and write is successful, false if pipe is closed
   *              or we are not in WRITE mode
   */
  bool write(const void* data, size_t size) {

    // check we are opened in write mode
    if (mode_ != mode::WRITE) {
      return false;
    }

    uint8_t *ptr = (uint8_t *) data;

    // try to write bytes
    std::unique_lock<decltype(wmutex_)> lock(wmutex_, std::defer_lock);
    for (size_t i = 0; i < size; ++i) {
      consumer_.wait();  // wait until there is space in the pipe

      // write next byte and advance write index
      lock.lock();
      int pos = info_->write;

      // check for EOF
      if (info_->weof > 0) {
        // Do not start write if pipe is closed, otherwise finish writing
        if ( (i == 0)  // start
             || (pos == 0 && info_->weof == info_->size)  // wrap
             || (pos == info_->weof)) {                   // regular
          consumer_.notify();  // notify any other writer threads
          return false;
        }
      }

      // next byte location to write, wrapping around if need be
      if ((++(info_->write)) == info_->size) {
        info_->write = 0;
      }
      lock.unlock();

      // do actual write outside of lock for efficiency
      // (though here it is only one byte...)
      pipe_[pos] = *ptr;
      ++ptr;

      producer_.notify();  // byte available for read
    }

    return true;
  }

  template<typename T>
  bool write(const T& data) {
    return this->write<T>(&data);
  }

  template<typename T>
  bool write(const T* data) {
    return this->write((void*)data, sizeof(T));
  }

  /**
   * Reads data from the pipe
   * @param data memory address to fill with pipe contents
   * @param size number of bytes
   * @return true if successful, false if not opened in read mode, or pipe is closed
   *              and does not have enough bytes left
   */
  bool read(void* data, size_t size) {

    // check we are the reader
    if (mode_ != mode::READ) {
      return false;
    }

    uint8_t *ptr = (uint8_t *) data;

    std::unique_lock<decltype(rmutex_)> lock(rmutex_, std::defer_lock);
    for (size_t i = 0; i < size; ++i) {
      producer_.wait();  // wait until there is data in the pipe

      // read next byte and advance read index
      lock.lock();
      size_t pos = info_->read;
      
      // check for EOF
      if (info_->reof > 0) {
        if ( (pos == 0 && info_->reof == info_->size)
             || (pos == info_->reof)) {
          producer_.notify();  // notify any other reader threads
          return false;
        }
      }
      
      // next byte location to read, wrapping around if need be
      if ((++(info_->read)) == info_->size) {
        info_->read = 0;
      }
      lock.unlock();

      // do the actualy write
      *ptr = pipe_[pos];
      ++ptr;  // advance ptr

      consumer_.notify();  // byte available for writing
    }

    return true;
  }

  /**
   * Read a single byte
   * @return next byte in the stream
   */
  uint8_t read() {
    uint8_t byte;
    this->read(&byte, 1);
    return byte;
  }

  template<typename T>
  bool read(T* data) {
    return read((void*)data, sizeof(T));
  }

  size_t available() {
    // lock both read and write to get indices, don't want them changing between here
    std::lock_guard<decltype(rmutex_)> rlock(rmutex_);
    std::lock_guard<decltype(wmutex_)> wlock(wmutex_);
    auto r = info_->read;
    auto w = info_->write;
    if (w < r) {
      return info_->size-r+w;
    }
    return w-r;
  }

  void close() {
    std::unique_lock<decltype(omutex_)> lock(omutex_);
    switch (mode_) {
      case READ: {
        info_->has_reader = false;
        lock.unlock();
        {
          std::lock_guard<decltype(rmutex_)> rlock(rmutex_);
          std::lock_guard<decltype(rmutex_)> wlock(wmutex_);
          // mark EOF
          auto r = info_->read;
          if (r == 0) {
            info_->weof = info_->size;
          } else {
            info_->weof = info_->read;
          }
        }
        consumer_.notify();
        break;
      }
      case WRITE: {
        info_->has_writer = false;
        lock.unlock();  // release
        {
          // note: we may need to wake up any reader
          std::lock_guard<decltype(rmutex_)> rlock(rmutex_);
          std::lock_guard<decltype(rmutex_)> wlock(wmutex_);
          // mark EOF
          auto w = info_->write;
          if (w == 0) {
            info_->reof = info_->size;
          } else {
            info_->reof = info_->write;
          }
        }
        producer_.notify();
        break;
      }
    }

  }

  bool unlink() {
    bool b1 = wmutex_.unlink();
    bool b2 = rmutex_.unlink();
    bool b3 = info_.unlink();
    bool b4 = pipe_.unlink();
    bool b5 = producer_.unlink();
    bool b6 = consumer_.unlink();
    return b1 && b2 && b3 && b4 && b5 && b6;
  }

  static bool unlink(const std::string& name) {

    bool b1 = cpen333::process::mutex::unlink(name + std::string(PIPE_WRITE_SUFFIX));
    bool b2 = cpen333::process::mutex::unlink(name + std::string(PIPE_READ_SUFFIX));
    bool b3 = cpen333::process::shared_object<pipe_info>::unlink(name + std::string(PIPE_INFO_SUFFIX));
    bool b4 = cpen333::process::shared_memory::unlink(name + std::string(PIPE_NAME_SUFFIX));
    bool b5 = cpen333::process::semaphore::unlink(name + std::string(PIPE_WRITE_SUFFIX));
    bool b6 = cpen333::process::semaphore::unlink(name + std::string(PIPE_READ_SUFFIX));

    return b1 && b2 && b3 && b4 && b5 && b6;
  }

 private:
  struct pipe_info {
    int initialized;
    size_t read;
    size_t write;
    size_t size;
    bool reof;
    bool weof;
    bool has_reader;
    bool has_writer;
  };

  mode mode_;
  cpen333::process::mutex wmutex_;
  cpen333::process::mutex rmutex_;
  cpen333::process::mutex omutex_;  // for checking if pipe is open
  cpen333::process::shared_object<pipe_info> info_;
  cpen333::process::shared_memory pipe_;
  cpen333::process::semaphore producer_;
  cpen333::process::semaphore consumer_;

};

}
}

#endif //CPEN333_PROCESS_PIPE_H
