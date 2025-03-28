#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

class Coroutine;
class Scheduler;
void coroutine_entry(Coroutine *coro, std::jmp_buf *scheduler_context);

class Coroutine {
public:
  enum class State { READY, RUNNING, AWAITING, DONE };

  using CoroutineFunc = std::function<void(Coroutine *)>;

  Coroutine(Scheduler *scheduler, CoroutineFunc func,
            size_t stack_size = 4096 * 4)
      : scheduler_(scheduler), func_(func), stack_size_(stack_size),
        state_(State::READY) {
    stack_ = malloc(stack_size_);
    if (!stack_) {
      std::cerr << "Failed to allocate stack for coroutine!" << std::endl;
      exit(1);
    }
  }

  ~Coroutine() { free(stack_); }

  Coroutine(const Coroutine &) = delete;
  Coroutine &operator=(const Coroutine &) = delete;

  void yield() {
    if (!setjmp(context_)) {
      longjmp(*scheduler_context_, 1);
    }
  }

  void run(std::jmp_buf *scheduler_context) {
    scheduler_context_ = scheduler_context;
    if (!setjmp(context_)) {
      yield();
    }
    state_ = State::RUNNING;
    func_(this);
    state_ = State::DONE;
    yield();
  }

  State get_state() const { return state_; }
  void set_state(State state) { state_ = state; }

  void sleep_for(std::chrono::milliseconds duration) {
    wake_time_ = std::chrono::steady_clock::now() + duration;
    state_ = State::AWAITING;
    yield();
  }

  std::chrono::steady_clock::time_point get_wake_time() const {
    return wake_time_;
  }

  std::jmp_buf *get_context() { return &context_; }
  void *get_stack() const { return stack_; }
  size_t get_stack_size() const { return stack_size_; }

private:
  Scheduler *scheduler_;
  CoroutineFunc func_;
  void *stack_;
  size_t stack_size_;
  std::jmp_buf context_;
  std::jmp_buf *scheduler_context_;
  volatile State state_;
  std::chrono::steady_clock::time_point wake_time_;
};

class Scheduler {
public:
  void add_coroutine(Coroutine::CoroutineFunc func) {
    coroutines_.push_back(std::make_unique<Coroutine>(this, func));
  }

  void run() {
    std::jmp_buf scheduler_context;
    size_t current_index = 0;

    // Initialize all coroutines
    for (auto &coro : coroutines_) {
      char *stack_top = (char *)coro->get_stack() + coro->get_stack_size();
      stack_top = (char *)((uintptr_t)stack_top & ~0xF);

      if (!setjmp(scheduler_context)) {
        switch_stack(stack_top, coro.get(), &scheduler_context);
      }
    }

    while (!all_finished()) {
      // Check and wake up sleeping coroutines
      auto now = std::chrono::steady_clock::now();
      for (auto &coro : coroutines_) {
        if (coro->get_state() == Coroutine::State::AWAITING &&
            coro->get_wake_time() <= now) {
          coro->set_state(Coroutine::State::READY);
        }
      }

      // Find next READY coroutine
      bool executed = false;
      for (size_t i = 0; i < coroutines_.size(); ++i) {
        auto &coro = coroutines_[current_index];
        if (coro->get_state() == Coroutine::State::READY) {
          coro->set_state(Coroutine::State::RUNNING);

          if (!setjmp(scheduler_context)) {
            longjmp(*(coro->get_context()), 1);
          }

          // After coroutine yields
          if (coro->get_state() == Coroutine::State::RUNNING) {
            coro->set_state(Coroutine::State::READY);
          }

          executed = true;
          current_index = (current_index + 1) % coroutines_.size();
          break;
        }
        current_index = (current_index + 1) % coroutines_.size();
      }

      if (!executed && !all_finished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    std::cout << "All coroutines finished" << std::endl;
  }

private:
  std::vector<std::unique_ptr<Coroutine>> coroutines_;

  bool all_finished() const {
    for (const auto &coro : coroutines_) {
      if (coro->get_state() != Coroutine::State::DONE) {
        return false;
      }
    }
    return true;
  }

  void switch_stack(void *new_sp, Coroutine *coro,
                    std::jmp_buf *scheduler_context) {
    void *old_sp;
    asm volatile("mov %%rsp, %0" : "=r"(old_sp));
    asm volatile("mov %0, %%rsp" : : "r"(new_sp));
    coroutine_entry(coro, scheduler_context);
    asm volatile("mov %0, %%rsp" : : "r"(old_sp));
  }
};

void coroutine_entry(Coroutine *coro, std::jmp_buf *scheduler_context) {
  coro->run(scheduler_context);
}
