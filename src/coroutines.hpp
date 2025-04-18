#pragma once

//-----------------------------------------------------------------------------
// Standard C++ Library Includes
//-----------------------------------------------------------------------------
#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

//-----------------------------------------------------------------------------
// Boost Includes
//-----------------------------------------------------------------------------
#include <boost/context/fiber.hpp>
#include <boost/context/stack_context.hpp>
#include <boost/context/stack_traits.hpp>

//-----------------------------------------------------------------------------
// System/Platform Includes (Linux Specific)
//-----------------------------------------------------------------------------
#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#error                                                                         \
    "This coroutine library currently requires Linux for epoll and mmap/mprotect stack allocation features."
#endif

namespace co_lib {

namespace ctx = boost::context;

class Scheduler;

#ifdef __linux__
//-----------------------------------------------------------------------------
// Custom Stack Allocator with Pooling and Guard Pages (Linux Specific)
//-----------------------------------------------------------------------------

/// @brief Allocates stacks for Boost.Context fibers using mmap.
///
/// Implements pooling for reuse and adds a protected guard page below the stack
/// to detect stack overflows via segmentation faults.
/// @note This implementation is Linux-specific due to mmap/mprotect usage.
class GuardedPooledStackAllocator {
private:
  std::size_t page_size_;
  std::size_t default_stack_size_;

  // Internal structure to hold allocation details
  struct StackInfo {
    void *allocation_start = nullptr;
    std::size_t total_size = 0;
    ctx::stack_context sctx{};
  };

  std::map<std::size_t, std::list<StackInfo>> free_stacks_;
  std::mutex pool_mutex_;

  /// @brief Aligns the given size up to the nearest page boundary.
  std::size_t page_align(std::size_t size) const {
    return (size + page_size_ - 1) & ~(page_size_ - 1);
  }

public:
  /// @brief Constructor. Determines page size and default stack size.
  /// @param default_stack_size Desired default usable stack size (will be page
  /// aligned).
  GuardedPooledStackAllocator(std::size_t default_stack_size =
                                  boost::context::stack_traits::default_size())
      : page_size_(0), default_stack_size_(0) {
    long ret = sysconf(_SC_PAGESIZE);
    if (ret == -1) {
      perror("sysconf(_SC_PAGESIZE) failed");
      page_size_ = 4096;
      std::cerr
          << "Warning: sysconf(_SC_PAGESIZE) failed. Using fallback page size: "
          << page_size_ << std::endl;
    } else {
      page_size_ = static_cast<std::size_t>(ret);
    }

    // Ensure default stack size is at least the minimum required and page
    // aligned
    std::size_t min_size = boost::context::stack_traits::minimum_size();
    default_stack_size_ = std::max(default_stack_size, min_size);
    default_stack_size_ = page_align(default_stack_size_);

    // std::cout << "Stack Allocator Initialized: Page Size=" << page_size_
    //           << ", Default Usable Stack Size=" << default_stack_size_
    //           << std::endl;
  }

  /// @brief Destructor. Cleans up all memory held in the pool.
  ~GuardedPooledStackAllocator() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (auto &pair : free_stacks_) {
      for (auto &info : pair.second) {
        mprotect(info.allocation_start, page_size_, PROT_READ | PROT_WRITE);
        munmap(info.allocation_start, info.total_size);
      }
    }
    free_stacks_.clear();
  }

  GuardedPooledStackAllocator(const GuardedPooledStackAllocator &) = delete;
  GuardedPooledStackAllocator &
  operator=(const GuardedPooledStackAllocator &) = delete;
  GuardedPooledStackAllocator(GuardedPooledStackAllocator &&) = delete;
  GuardedPooledStackAllocator &
  operator=(GuardedPooledStackAllocator &&) = delete;

  /// @brief Allocates stack context. Uses pooling if available, otherwise mmap.
  /// @param requested_size The desired usable stack size. If 0, uses the
  /// allocator's default.
  /// @return A boost::context::stack_context object. Throws on failure.
  ctx::stack_context allocate(std::size_t requested_size = 0) {
    // Determine actual stack size, ensuring minimum size and page alignment
    std::size_t stack_size = page_align(
        (requested_size == 0) ? default_stack_size_ : requested_size);
    if (stack_size < boost::context::stack_traits::minimum_size()) {
      stack_size = page_align(boost::context::stack_traits::minimum_size());
    }

    StackInfo info;
    bool reused = false;

    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      auto it = free_stacks_.find(stack_size);
      if (it != free_stacks_.end() && !it->second.empty()) {
        info = it->second.front();
        it->second.pop_front();
        reused = true;
      }
    } // Mutex scope ends

    // --- If reused from pool, re-protect guard page ---
    if (reused) {
      if (mprotect(info.allocation_start, page_size_, PROT_NONE) == -1) {
        perror("mprotect PROT_NONE failed on pooled stack reuse");
        munmap(info.allocation_start, info.total_size);
        reused = false; // Fall through to allocate new
      } else {
        return info.sctx; // Return pooled stack context
      }
    }

    // --- Pool empty or reuse failed, allocate new via mmap ---
    std::size_t guard_size = page_size_;
    std::size_t total_size = stack_size + guard_size;

    void *allocation_start =
        mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);

    if (allocation_start == MAP_FAILED) {
      perror("mmap failed");
      throw std::bad_alloc(); // Indicate allocation failure
    }

    // Protect the guard page (the lowest address page in the allocation)
    if (mprotect(allocation_start, guard_size, PROT_NONE) == -1) {
      perror("mprotect PROT_NONE failed on new stack");
      munmap(allocation_start, total_size); // Clean up allocation
      throw std::runtime_error("Failed to protect stack guard page");
    }

    // Calculate usable stack context
    void *stack_limit = static_cast<char *>(allocation_start) + guard_size;
    void *stack_base = static_cast<char *>(stack_limit) + stack_size;

    ctx::stack_context sctx;
    sctx.sp = static_cast<char *>(stack_base);
    sctx.size = stack_size;

    return sctx;
  }

  /// @brief Deallocates stack context, returning it to the pool.
  /// @param sctx The stack context previously allocated by allocate(). Will be
  /// invalidated.
  void deallocate(ctx::stack_context &sctx) noexcept {
    // Check for invalid context (null sp or zero size)
    if (!sctx.sp || sctx.size == 0) {
      return;
    }

    const std::size_t stack_size = sctx.size;
    const void *stack_base = sctx.sp;
    const void *stack_limit =
        static_cast<const char *>(stack_base) - stack_size;
    void *allocation_start =
        static_cast<char *>(const_cast<void *>(stack_limit)) - page_size_;
    const std::size_t total_size = stack_size + page_size_; // Total mmap'd size

    if (mprotect(allocation_start, page_size_, PROT_READ | PROT_WRITE) == -1) {
      perror("mprotect RW failed on deallocate");
      munmap(allocation_start, total_size);
    } else {
      // Prepare StackInfo to add back to pool
      StackInfo info;
      info.allocation_start = allocation_start;
      info.total_size = total_size;
      info.sctx = sctx;

      // Add to the appropriate pool based on usable size
      {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        free_stacks_[stack_size].push_back(info);
      }
    }

    sctx.sp = nullptr;
    sctx.size = 0;
  }
};

/// @brief Provides access to a shared instance of the stack allocator.
/// @return Reference to the GuardedPooledStackAllocator instance.
inline GuardedPooledStackAllocator &get_stack_allocator() {
  static GuardedPooledStackAllocator allocator_instance(128 * 1024);
  return allocator_instance;
}

/// @brief A stack allocator adapter used per-coroutine for Boost.Context fiber
/// construction.
/// @details Stores the desired stack size and delegates to the shared pool
/// allocator.
class PerCoroutineStackAllocator {
private:
  GuardedPooledStackAllocator &pool_allocator_;
  std::size_t stack_size_;

public:
  /// @brief Constructor.
  /// @param pool A reference to the shared GuardedPooledStackAllocator.
  /// @param size The desired usable stack size for the coroutine. If 0, uses
  /// pool's default.
  PerCoroutineStackAllocator(GuardedPooledStackAllocator &pool,
                             std::size_t size) noexcept
      : pool_allocator_(pool), stack_size_(size) {}

  /// @brief Allocates stack context by delegating to the pool allocator with
  /// the stored size.
  /// @return Allocated stack context.
  ctx::stack_context allocate() {
    return pool_allocator_.allocate(stack_size_);
  }

  /// @brief Deallocates stack context by delegating to the pool allocator.
  /// @param sctx The stack context to deallocate.
  void deallocate(ctx::stack_context &sctx) noexcept {
    pool_allocator_.deallocate(sctx);
  }
};
#endif // __linux__

//-----------------------------------------------------------------------------
// CoroutineCancelledException
//-----------------------------------------------------------------------------

/// @brief Exception thrown when an operation is performed on a cancelled
/// coroutine,
///        or when a yield point detects a cancellation request.
class CoroutineCancelledException : public std::runtime_error {
public:
  CoroutineCancelledException()
      : std::runtime_error("Coroutine was cancelled") {}
};

//-----------------------------------------------------------------------------
// Coroutine Class Definition
//-----------------------------------------------------------------------------

/// @brief Represents a cooperatively scheduled task (coroutine) with its own
/// stack.
/// @details Uses Boost.Context for context switching and a custom pooled
/// allocator
///          with guard pages on Linux. Allows yielding, sleeping, waiting for
///          I/O (Linux), returning values, joining, and cancellation.
class Coroutine {
public:
  /// @brief Possible states of a Coroutine.
  enum class State {
    READY,          ///< Ready to run.
    RUNNING,        ///< Currently executing.
    AWAITING_TIMER, ///< Paused, waiting for a specific time point.
    AWAITING_IO,    ///< Paused, waiting for a file descriptor I/O event (Linux
                    ///< epoll).
    // AWAITING_JOIN, ///< State for advanced join (not used in simple polling
    // join)
    DONE,  ///< Execution finished successfully.
    FAILED ///< Execution terminated due to an exception or error.
  };

  /// @brief Function signature for the code executed by the coroutine.
  using CoroutineFunc = std::function<void(Coroutine *)>;

  /// @brief Constructs a new Coroutine using the custom stack allocator.
  /// @param scheduler Pointer to the Scheduler managing this coroutine.
  /// @param func The function the coroutine will execute.
  /// @param stack_size The desired *usable* stack size. If 0, uses the
  /// allocator's default.
  /// @throws std::runtime_error or std::bad_alloc on stack allocation failure
  /// or if run on non-Linux.
  Coroutine(Scheduler *scheduler, CoroutineFunc func, size_t stack_size = 0)
      : scheduler_(scheduler), func_(std::move(func)), state_(State::READY),
        waiting_fd_(-1), context_{} {
#ifdef __linux__
    // 1. Create the per-coroutine allocator wrapper instance, passing the
    // shared pool
    //    and the desired stack size for THIS coroutine.
    PerCoroutineStackAllocator current_coro_alloc(get_stack_allocator(),
                                                  stack_size);

    // 2. Initialize the fiber using the standard allocator constructor.
    //    The fiber will call current_coro_alloc.allocate() internally.
    //    The fiber's destructor will call current_coro_alloc.deallocate().
    context_ = ctx::fiber(std::allocator_arg, current_coro_alloc,

                          [this](ctx::fiber &&main_context) {
                            caller_context_ = std::move(main_context);
                            try {
                              check_cancellation();
                              func_(this);
                              state_ = State::DONE;
                            } catch (const CoroutineCancelledException &) {
                              state_ = State::FAILED;
                              captured_exception_ = std::current_exception();
                            } catch (const std::exception &e) {
                              std::cerr
                                  << "Coroutine caught exception: " << e.what()
                                  << std::endl;
                              captured_exception_ = std::current_exception();
                              state_ = State::FAILED;
                            } catch (...) {
                              std::cerr << "Coroutine caught unknown exception."
                                        << std::endl;
                              captured_exception_ = std::current_exception();
                              state_ = State::FAILED;
                            }
                            if (waiting_fd_ != -1) {
                              waiting_fd_ = -1;
                            }
                            return std::move(caller_context_);
                          }); // End fiber constructor
#else
    // Explicitly prevent construction on non-Linux systems
    (void)scheduler;
    (void)func;
    (void)stack_size; // Suppress unused warnings
    throw std::runtime_error(
        "Coroutine requires Linux stack allocation support (mmap/mprotect).");
#endif
  }

  /// @brief Destructor. Fiber destructor handles stack deallocation via its
  /// allocator.
  ~Coroutine() = default;

  Coroutine(const Coroutine &) = delete;
  Coroutine &operator=(const Coroutine &) = delete;
  Coroutine(Coroutine &&) = delete;
  Coroutine &operator=(Coroutine &&) = delete;

  /// @brief Yields execution control back to the scheduler.
  /// @throws CoroutineCancelledException If cancellation has been requested.
  void yield() {
    check_cancellation();

    if (state_ == State::RUNNING) {
      state_ = State::READY;
    }

    if (caller_context_) {
      caller_context_ = std::move(caller_context_).resume();
    } else {
      state_ = State::FAILED;
      throw std::runtime_error(
          "Internal Error: Coroutine yielding without valid caller context!");
    }

    if (state_ != State::DONE && state_ != State::FAILED) {
      state_ = State::RUNNING;
    }
    check_cancellation();
  }

  /// @brief Gets the current state of the coroutine.
  State get_state() const noexcept { return state_; }

  /// @brief Gets the time point at which a sleeping coroutine should wake up.
  std::chrono::steady_clock::time_point get_wake_time() const noexcept {
    return wake_time_;
  }

  /// @brief Gets the file descriptor the coroutine is waiting on (if any).
  int get_waiting_fd() const noexcept { return waiting_fd_; }

  /// @brief Gets the captured exception pointer if the coroutine FAILED.
  std::exception_ptr get_exception_ptr() const noexcept {
    return captured_exception_;
  }

  /// @brief Pauses the coroutine for a specified duration.
  /// @param duration The minimum duration to sleep for.
  /// @throws CoroutineCancelledException If cancellation has been requested.
  void sleep_for(std::chrono::milliseconds duration) {
    check_cancellation();
    wake_time_ = std::chrono::steady_clock::now() + duration;
    state_ = State::AWAITING_TIMER;
    waiting_fd_ = -1;
    yield();
  }

  /// @brief Pauses until the specified FD is ready for reading (Linux only).
  /// @param fd The non-blocking file descriptor to wait on.
  /// @throws CoroutineCancelledException If cancellation has been requested.
  /// @throws std::invalid_argument If fd is invalid.
  /// @throws std::runtime_error If FD registration fails.
  void await_read(int fd) { await_fd(fd, EPOLLIN); }

  /// @brief Pauses until the specified FD is ready for writing (Linux only).
  /// @param fd The non-blocking file descriptor to wait on.
  /// @throws CoroutineCancelledException If cancellation has been requested.
  /// @throws std::invalid_argument If fd is invalid.
  /// @throws std::runtime_error If FD registration fails.
  void await_write(int fd) { await_fd(fd, EPOLLOUT); }

  /// @brief Sets the return value for this coroutine (to be retrieved after
  /// DONE state).
  /// @tparam T Type of the value (must be copyable/movable for std::any).
  /// @param value The value to store.
  template <typename T> void set_return_value(T &&value) {
    if (state_ != State::DONE && state_ != State::FAILED) {
      return_value_ = std::forward<T>(value);
    }
  }

  /// @brief Gets the return value, attempting to cast it to type T.
  /// @tparam T The expected type of the return value.
  /// @return The stored return value cast to type T.
  /// @throws std::runtime_error If called before coroutine is DONE.
  /// @throws std::bad_any_cast If stored value cannot be cast to T or no value
  /// was set.
  template <typename T> T get_return_value() const {
    if (state_ != State::DONE) {
      throw std::runtime_error(
          "Return value only available when coroutine is DONE.");
    }
    if (!return_value_.has_value()) {
      throw std::bad_any_cast();
    }
    try {
      return std::any_cast<T>(return_value_);
    } catch (const std::bad_any_cast &e) {
      throw;
    }
  }

  /// @brief Gets the return value as std::any without casting.
  /// @return The stored std::any object (may be empty if no value was set).
  /// @throws std::runtime_error If called before coroutine is DONE.
  const std::any &get_return_value_any() const {
    if (state_ != State::DONE) {
      throw std::runtime_error(
          "Return value only available when coroutine is DONE.");
    }
    return return_value_;
  }

  /// @brief Waits until another coroutine finishes (DONE or FAILED state).
  /// @details This is a simple polling implementation using yield().
  /// @param target_coro Non-null pointer to the coroutine to wait for (must not
  /// be `this`).
  /// @throws std::invalid_argument If target_coro is null or this.
  /// @throws CoroutineCancelledException If this coroutine is cancelled while
  /// waiting.
  void join(Coroutine *target_coro) {
    if (!target_coro || target_coro == this) {
      throw std::invalid_argument("Invalid target coroutine for join.");
    }
    check_cancellation();

    while (true) {
      Coroutine::State target_state = target_coro->get_state();
      if (target_state == State::DONE || target_state == State::FAILED) {
        break; // Target finished
      }
      yield();
    }
    // Optional: check target_coro state here if needed (e.g., throw if it
    // FAILED?) if (target_coro->get_state() == State::FAILED) { ... }
  }

  /// @brief Sets a flag requesting this coroutine to stop execution.
  void request_cancellation() noexcept {
    cancellation_requested_.store(true, std::memory_order_relaxed);
  }

  /// @brief Checks if cancellation has been requested.
  bool is_cancellation_requested() const noexcept {
    return cancellation_requested_.load(std::memory_order_relaxed);
  }

private:
  friend class Scheduler;

  /// @brief Waits for specific I/O events on a file descriptor (Linux epoll).
  /// @param fd The non-blocking file descriptor.
  /// @param events The epoll events (e.g., EPOLLIN, EPOLLOUT) to wait for.
  /// @throws CoroutineCancelledException If cancellation requested before
  /// yielding.
  /// @throws std::invalid_argument If fd < 0.
  /// @throws std::runtime_error If FD registration with epoll fails.
  void await_fd(int fd, uint32_t events); // Defined after Scheduler

  /// @brief Throws CoroutineCancelledException if cancellation has been
  /// requested.
  void check_cancellation() const {
    if (is_cancellation_requested()) {
      throw CoroutineCancelledException();
    }
  }

  /// @brief Internal method called by Scheduler when I/O is ready.
  void set_io_ready() noexcept {
    if (state_ == State::AWAITING_IO) {
      waiting_fd_ = -1; // Clear waiting fd
      state_ = State::READY;
    }
  }

  /// @brief Internal method for Scheduler to set state (e.g., for timers). Use
  /// with care.
  void set_state(State state) noexcept { state_ = state; }

  // --- Member Variables ---
  Scheduler *scheduler_;
  CoroutineFunc func_;

  ctx::fiber context_;
  ctx::fiber caller_context_;

  State state_;
  std::chrono::steady_clock::time_point wake_time_;
  int waiting_fd_;
  uint32_t waiting_events_;
  std::exception_ptr captured_exception_{nullptr};
  std::any return_value_;
  std::atomic<bool> cancellation_requested_{false}; // Cancellation flag
};

//-----------------------------------------------------------------------------
// Scheduler Class Definition
//-----------------------------------------------------------------------------

/// @brief Manages and executes a collection of Coroutines cooperatively using
/// epoll (Linux).
class Scheduler {
public:
  /// @brief Default constructor.
  Scheduler() : epoll_fd_(-1) {}

  /// @brief Destructor. Cleans up epoll.
  ~Scheduler() {
#ifdef __linux__
    if (epoll_fd_ != -1) {
      close(epoll_fd_);
    }
#endif
    // Coroutines are destroyed via unique_ptr when Scheduler is destroyed
    // Shared stack pool is cleaned up when its static instance goes out of
    // scope
  }

  // Non-copyable, non-movable
  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(Scheduler &&) = delete;

  /// @brief Adds a new coroutine to the scheduler.
  /// @param func The function the coroutine will execute.
  /// @param stack_size The desired usable stack size (0 for default).
  /// @return Coroutine* A non-owning pointer to the created coroutine. The
  /// caller
  ///                   must not delete this pointer; the Scheduler owns the
  ///                   Coroutine. Returns nullptr if coroutine creation fails.
  /// @throws std::runtime_error or std::bad_alloc from Coroutine constructor on
  /// failure.
  Coroutine *add_coroutine(Coroutine::CoroutineFunc func,
                           size_t stack_size = 0) {
    coroutines_.push_back(
        std::make_unique<Coroutine>(this, std::move(func), stack_size));
    return coroutines_.back().get();
  }

  /// @brief Starts the scheduler's event loop (Linux epoll implementation).
  /// @details Blocks the calling thread until all coroutines complete or fail.
  void run() {
#ifdef __linux__
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) {
      perror("epoll_create1 failed");
      throw std::runtime_error("Failed to initialize epoll for scheduler.");
    }

    const int MAX_EVENTS = 16;
    struct epoll_event events[MAX_EVENTS];
    size_t current_index = 0; // Index for round-robin search

    while (!all_finished()) {
      int timeout_ms = calculate_epoll_timeout();
      int n_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);

      if (n_events < 0) {
        if (errno == EINTR)
          continue;
        perror("epoll_wait failed");
        break;
      }

      auto now = std::chrono::steady_clock::now();

      // Process I/O Events
      for (int i = 0; i < n_events; ++i) {
        Coroutine *coro = static_cast<Coroutine *>(events[i].data.ptr);
        if (coro && coro->get_state() == Coroutine::State::AWAITING_IO) {
          coro->set_io_ready();
        }
      }

      // Check Timers
      for (auto &coro_ptr : coroutines_) {
        if (coro_ptr->get_state() == Coroutine::State::AWAITING_TIMER &&
            coro_ptr->get_wake_time() <= now) {
          coro_ptr->set_state(Coroutine::State::READY);
        }
      }

      size_t checked_count = 0;
      size_t num_coroutines = coroutines_.size();
      while (checked_count < num_coroutines) {
        if (current_index >= coroutines_.size())
          current_index = 0;
        if (coroutines_.empty())
          break;

        Coroutine *coro_ptr = coroutines_[current_index].get();
        size_t next_index = (current_index + 1) % coroutines_.size();

        if (coro_ptr->get_state() == Coroutine::State::READY) {

          ctx::fiber &coro_fiber_member = coro_ptr->context_;

          if (coro_fiber_member) {
            ctx::fiber returned_fiber = std::move(coro_fiber_member).resume();
            if (returned_fiber) {
              coro_ptr->context_ = std::move(returned_fiber);
            } else {
            }
          } else {
            std::cerr << "Scheduler Error: Attempting to resume an invalid "
                         "fiber context!"
                      << std::endl;
            coro_ptr->set_state(Coroutine::State::FAILED);
          }

          Coroutine::State final_state = coro_ptr->get_state();
          if (final_state == Coroutine::State::FAILED ||
              final_state == Coroutine::State::DONE) {
          }

          current_index = next_index;
          checked_count++;
          break;
        } else {
          current_index = next_index;
          checked_count++;
        }
      } // End inner round-robin loop
    } // End outer while(!all_finished)

    close(epoll_fd_);
    epoll_fd_ = -1;

#else  // !__linux__
    std::cerr << "Warning: Scheduler::run() called on non-Linux platform. "
                 "Epoll features disabled."
              << std::endl;
    if (coroutines_.empty()) {
      std::cerr << "(No coroutines were added.)" << std::endl;
    } else {
      std::cerr << "(" << coroutines_.size()
                << " coroutine(s) were added but cannot be run.)" << std::endl;
    }
#endif // __linux__
  }

private:
  friend class Coroutine;

  /// @brief Registers FD with epoll (Linux only).
  bool register_fd(Coroutine *coro, int fd, uint32_t events) {
#ifdef __linux__
    struct epoll_event ev{};
    ev.events = events | EPOLLET | EPOLLONESHOT;
    ev.data.ptr = coro;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
      if (errno == EEXIST) {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
          perror("epoll_ctl MOD failed");
          return false;
        }
      } else {
        perror("epoll_ctl ADD failed");
        return false;
      }
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
      perror("fcntl F_GETFL failed");
      epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL);
      return false;
    }
    if (!(flags & O_NONBLOCK)) {
      if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK failed");
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL);
        return false;
      }
    }
    return true;
#else
    (void)coro;
    (void)fd;
    (void)events;
    return false;
#endif
  }

  /// @brief Unregisters FD from epoll (Linux only).
  void unregister_fd(int fd) {
#ifdef __linux__
    if (epoll_fd_ != -1 && fd != -1) {
      epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL); // Ignore errors
    }
#else
    (void)fd;
#endif
  }

  /// @brief Checks if all coroutines are finished (DONE or FAILED).
  bool all_finished() const noexcept {
    if (coroutines_.empty()) {
      return false;
    }
    for (const auto &coro : coroutines_) {
      Coroutine::State state = coro->get_state();
      if (state != Coroutine::State::DONE &&
          state != Coroutine::State::FAILED) {
        return false;
      }
    }
    return true;
  }

  /// @brief Calculates epoll timeout based on earliest timer.
  int calculate_epoll_timeout() noexcept {
    int timeout_ms = -1;
    auto now = std::chrono::steady_clock::now();
    auto next_wake = std::chrono::steady_clock::time_point::max();
    bool found_timer = false;
    bool found_ready = false;

    for (const auto &coro : coroutines_) {
      Coroutine::State state = coro->get_state();
      if (state == Coroutine::State::AWAITING_TIMER) {
        found_timer = true;
        if (coro->get_wake_time() < next_wake) {
          next_wake = coro->get_wake_time();
        }
      } else if (state == Coroutine::State::READY) {
        found_ready = true;
      }
    }

    // If any coroutine is ready, don't block in epoll_wait
    if (found_ready) {
      return 0;
    }

    // If timers are pending, calculate the duration until the next one expires
    if (found_timer) {
      if (next_wake <= now) {
        timeout_ms = 0; // Timer already expired
      } else {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            next_wake - now);
        long long ms_count = duration.count();
        constexpr long long max_int =
            static_cast<long long>(std::numeric_limits<int>::max());
        timeout_ms = (ms_count > max_int) ? std::numeric_limits<int>::max()
                                          : static_cast<int>(ms_count);
        if (timeout_ms < 0)
          timeout_ms = 0; // Ensure non-negative
      }
    }
    // If no timers and nothing ready, timeout_ms remains -1 (infinite wait)

    return timeout_ms;
  }

  std::vector<std::unique_ptr<Coroutine>> coroutines_;
  int epoll_fd_ = -1;
};

//-----------------------------------------------------------------------------
// Coroutine Method Definitions (Requiring Scheduler Definition)
//-----------------------------------------------------------------------------

/// @brief Implementation of Coroutine::await_fd. Defined here because it uses
/// Scheduler::register_fd.
inline void Coroutine::await_fd(int fd, uint32_t events) {
  check_cancellation();

  if (fd < 0) {
    throw std::invalid_argument("Invalid file descriptor passed to await_fd.");
  }

  // Attempt to register the FD with the scheduler's epoll instance
  if (scheduler_->register_fd(this, fd, events)) {
    waiting_fd_ = fd;
    waiting_events_ = events;
    state_ = State::AWAITING_IO;
    yield();
  } else {
    state_ = State::FAILED;
    captured_exception_ = std::make_exception_ptr(std::runtime_error(
        "Await failed: Could not register file descriptor with epoll."));
    yield();
  }
}

} // namespace co_lib
