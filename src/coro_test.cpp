#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

class Scheduler;

template <typename ReturnType>
void coroutine_entry(void *coro_ptr, std::jmp_buf *scheduler_context);

class CoroutineBase {
public:
  enum class State { READY, RUNNING, AWAITING, DONE };

  CoroutineBase(Scheduler *scheduler, size_t stack_size = 4096 * 4)
      : scheduler_(scheduler), stack_size_(stack_size), state_(State::READY) {
    stack_ = malloc(stack_size_);
    if (!stack_) {
      std::cerr << "Failed to allocate stack for coroutine!" << std::endl;
      exit(1);
    }
  }
  virtual ~CoroutineBase() { free(stack_); }

  CoroutineBase(const CoroutineBase &) = delete;
  CoroutineBase &operator=(const CoroutineBase &) = delete;

  void yield() {
    if (!setjmp(context_)) {
      longjmp(*scheduler_context_, 1);
    }
  }

  virtual void run(std::jmp_buf *scheduler_context) = 0;

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

protected:
  Scheduler *scheduler_;
  void *stack_;
  size_t stack_size_;
  std::jmp_buf context_;
  std::jmp_buf *scheduler_context_;
  volatile State state_;
  std::chrono::steady_clock::time_point wake_time_;
};

// Templated Coroutine implementation with return type support
template <typename ReturnType = void> class Coroutine : public CoroutineBase {
public:
  using CoroutineFunc = std::function<ReturnType(Coroutine *)>;

  Coroutine(Scheduler *scheduler, CoroutineFunc func,
            size_t stack_size = 4096 * 4)
      : CoroutineBase(scheduler, stack_size), func_(func),
        has_returned_(false) {}

  void run(std::jmp_buf *scheduler_context) override {
    scheduler_context_ = scheduler_context;
    if (!setjmp(context_)) {
      yield();
    }
    state_ = State::RUNNING;

    if constexpr (std::is_void_v<ReturnType>) {
      func_(this);
    } else {
      return_value_ = func_(this);
    }

    has_returned_ = true;
    state_ = State::DONE;
    yield();
  }

  bool has_returned() const { return has_returned_; }

  const ReturnType &get_result() const {
    if (!has_returned_) {
      throw std::runtime_error("Coroutine hasn't completed yet");
    }
    return return_value_;
  }

private:
  CoroutineFunc func_;
  ReturnType return_value_;
  bool has_returned_;
};

// Specialization for void return type
template <> class Coroutine<void> : public CoroutineBase {
public:
  using CoroutineFunc = std::function<void(Coroutine *)>;

  Coroutine(Scheduler *scheduler, CoroutineFunc func,
            size_t stack_size = 4096 * 4)
      : CoroutineBase(scheduler, stack_size), func_(func),
        has_returned_(false) {}

  void run(std::jmp_buf *scheduler_context) override {
    scheduler_context_ = scheduler_context;
    if (!setjmp(context_)) {
      yield();
    }
    state_ = State::RUNNING;
    func_(this);
    has_returned_ = true;
    state_ = State::DONE;
    yield();
  }

  bool has_returned() const { return has_returned_; }

  void get_result() const {
    if (!has_returned_) {
      throw std::runtime_error("Coroutine hasn't completed yet");
    }
    // Nothing to return for void
  }

private:
  CoroutineFunc func_;
  bool has_returned_;
};

class Scheduler {
public:
  // Base wrapper for type erasure
  class CoroutineWrapper {
  public:
    virtual ~CoroutineWrapper() = default;
    virtual CoroutineBase *get_base() = 0;
    virtual bool has_returned() const = 0;
  };

  // Typed wrapper for retrieving specific coroutine results
  template <typename ReturnType>
  class TypedCoroutineWrapper : public CoroutineWrapper {
  public:
    TypedCoroutineWrapper(Coroutine<ReturnType> *coro) : coro_(coro) {}
    ~TypedCoroutineWrapper() override { delete coro_; }

    CoroutineBase *get_base() override { return coro_; }
    bool has_returned() const override { return coro_->has_returned(); }

    Coroutine<ReturnType> *get_typed_coroutine() { return coro_; }

  private:
    Coroutine<ReturnType> *coro_;
  };

  template <typename ReturnType = void>
  size_t add_coroutine(typename Coroutine<ReturnType>::CoroutineFunc func) {
    auto coro = new Coroutine<ReturnType>(this, func);
    auto wrapper = std::make_unique<TypedCoroutineWrapper<ReturnType>>(coro);
    coroutines_.push_back(std::move(wrapper));
    return coroutines_.size() - 1;
  }

  void run() {
    std::jmp_buf scheduler_context;
    size_t current_index = 0;

    // Initialize all coroutines
    for (auto &coro_wrapper : coroutines_) {
      auto coro = coro_wrapper->get_base();
      char *stack_top = (char *)coro->get_stack() + coro->get_stack_size();
      stack_top = (char *)((uintptr_t)stack_top & ~0xF);

      if (!setjmp(scheduler_context)) {
        switch_stack(stack_top, coro, &scheduler_context);
      }
    }

    while (!all_finished()) {
      // Check and wake up sleeping coroutines
      auto now = std::chrono::steady_clock::now();
      for (auto &coro_wrapper : coroutines_) {
        auto coro = coro_wrapper->get_base();
        if (coro->get_state() == CoroutineBase::State::AWAITING &&
            coro->get_wake_time() <= now) {
          coro->set_state(CoroutineBase::State::READY);
        }
      }

      // Find next READY coroutine
      bool executed = false;
      for (size_t i = 0; i < coroutines_.size(); ++i) {
        auto &coro_wrapper = coroutines_[current_index];
        auto coro = coro_wrapper->get_base();
        if (coro->get_state() == CoroutineBase::State::READY) {
          coro->set_state(CoroutineBase::State::RUNNING);

          if (!setjmp(scheduler_context)) {
            longjmp(*(coro->get_context()), 1);
          }

          // After coroutine yields
          if (coro->get_state() == CoroutineBase::State::RUNNING) {
            coro->set_state(CoroutineBase::State::READY);
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

  // Helper to get result from a specific coroutine
  template <typename ReturnType> ReturnType get_result(size_t index) {
    if (index >= coroutines_.size()) {
      throw std::out_of_range("Coroutine index out of range");
    }

    auto wrapper = dynamic_cast<TypedCoroutineWrapper<ReturnType> *>(
        coroutines_[index].get());
    if (!wrapper) {
      throw std::runtime_error("Incorrect coroutine return type");
    }

    return wrapper->get_typed_coroutine()->get_result();
  }

  // Helper to check if a coroutine has completed
  bool is_completed(size_t index) {
    if (index >= coroutines_.size()) {
      throw std::out_of_range("Coroutine index out of range");
    }
    return coroutines_[index]->has_returned();
  }

  std::vector<std::unique_ptr<CoroutineWrapper>> coroutines_;

private:
  bool all_finished() const {
    for (const auto &coro_wrapper : coroutines_) {
      auto coro = coro_wrapper->get_base();
      if (coro->get_state() != CoroutineBase::State::DONE) {
        return false;
      }
    }
    return true;
  }

  void switch_stack(void *new_sp, CoroutineBase *coro,
                    std::jmp_buf *scheduler_context) {
    void *old_sp;
    asm volatile("mov %%rsp, %0" : "=r"(old_sp));
    asm volatile("mov %0, %%rsp" : : "r"(new_sp));

    // Determine the correct entry function based on the actual coroutine type
    if (auto typed_coro = dynamic_cast<Coroutine<int> *>(coro)) {
      coroutine_entry<int>(typed_coro, scheduler_context);
    } else if (auto typed_coro = dynamic_cast<Coroutine<double> *>(coro)) {
      coroutine_entry<double>(typed_coro, scheduler_context);
    } else if (auto typed_coro = dynamic_cast<Coroutine<std::string> *>(coro)) {
      coroutine_entry<std::string>(typed_coro, scheduler_context);
    } else if (auto typed_coro = dynamic_cast<Coroutine<void> *>(coro)) {
      coroutine_entry<void>(typed_coro, scheduler_context);
    } else {
      // Default case - void
      coroutine_entry<void>(coro, scheduler_context);
    }

    asm volatile("mov %0, %%rsp" : : "r"(old_sp));
  }
};

// Templated coroutine entry function
template <typename ReturnType>
void coroutine_entry(void *coro_ptr, std::jmp_buf *scheduler_context) {
  auto coro = static_cast<Coroutine<ReturnType> *>(coro_ptr);
  coro->run(scheduler_context);
}

// Example usage
int main() {
  Scheduler scheduler;

  // Coroutine that returns an integer
  auto coro1_id = scheduler.add_coroutine<int>([](Coroutine<int> *coro) -> int {
    std::cout << "Coroutine 1 starting" << std::endl;
    coro->sleep_for(std::chrono::milliseconds(1000));
    std::cout << "Coroutine 1 woke up" << std::endl;
    coro->yield();
    std::cout << "Coroutine 1 resumed and finishing" << std::endl;
    return 42;
  });

  // Coroutine that returns a string
  auto coro2_id = scheduler.add_coroutine<std::string>(
      [](Coroutine<std::string> *coro) -> std::string {
        std::cout << "Coroutine 2 starting" << std::endl;
        coro->sleep_for(std::chrono::milliseconds(500));
        std::cout << "Coroutine 2 finishing" << std::endl;
        return "Hello from coroutine";
      });

  auto coro3_id = scheduler.add_coroutine<void>([](Coroutine<void> *coro) {
    std::cout << "Coroutine 3 starting" << std::endl;
    coro->sleep_for(std::chrono::milliseconds(300));
    std::cout << "Coroutine 3 finishing" << std::endl;
  });

  scheduler.run();

  // Get results after completion
  try {
    int result1 = scheduler.get_result<int>(coro1_id);
    std::string result2 = scheduler.get_result<std::string>(coro2_id);

    std::cout << "Coroutine 1 returned: " << result1 << std::endl;
    std::cout << "Coroutine 2 returned: " << result2 << std::endl;
    std::cout << "Coroutine 3 completed: "
              << (scheduler.is_completed(coro3_id) ? "yes" : "no") << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Error retrieving results: " << e.what() << std::endl;
  }

  return 0;
}
