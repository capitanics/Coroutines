#include "../src/co_lib.hpp"

#include <sys/timerfd.h>

using namespace co_lib;

void timer_coroutine(Coroutine *self) {
  std::cout << "Timer Coroutine: Starting." << std::endl;
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (tfd == -1) {
    perror("timerfd_create failed");
    return;
  }

  struct itimerspec ts{};
  ts.it_value.tv_sec = 2;
  ts.it_interval.tv_sec = 1;
  if (timerfd_settime(tfd, 0, &ts, NULL) == -1) {
    perror("timerfd_settime failed");
    close(tfd);
    return;
  }

  std::cout << "Timer Coroutine: Waiting on fd " << tfd << std::endl;
  for (int i = 0; i < 5; ++i) {
    self->await_read(tfd);
    if (self->get_state() == Coroutine::State::FAILED) {
      std::cerr << "Timer Coroutine: await_read failed." << std::endl;
      break;
    }

    uint64_t expirations;
    ssize_t s = read(tfd, &expirations, sizeof(expirations));
    if (s == sizeof(expirations)) {
      std::cout << "Timer Coroutine: Timer " << tfd << " expired "
                << expirations << " time(s) (Iter " << i + 1 << ")"
                << std::endl;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("read timerfd failed");
      break;
    } else {
      std::cout << "Timer Coroutine: Read would block (EAGAIN)." << std::endl;
    }
  }
  std::cout << "Timer Coroutine: Finished." << std::endl;
  close(tfd);
}

void sleeper_coroutine(Coroutine *self) {
  std::cout << "Sleeper Coroutine: Starting." << std::endl;
  for (int i = 0; i < 3; ++i) {
    std::cout << "Sleeper Coroutine: Sleeping for 1.5 seconds..." << std::endl;
    self->sleep_for(std::chrono::milliseconds(1500));
    if (self->get_state() == Coroutine::State::FAILED)
      break;
    std::cout << "Sleeper Coroutine: Woke up (Iteration " << i + 1 << ")"
              << std::endl;
  }
  if (self->get_state() != Coroutine::State::FAILED) {
    std::cout << "Sleeper Coroutine: Finished normally." << std::endl;
  } else {
    std::cout << "Sleeper Coroutine: Exited due to failure/exception."
              << std::endl;
  }
}

int main() {
  Scheduler scheduler;
  scheduler.add_coroutine(timer_coroutine);
  scheduler.add_coroutine(sleeper_coroutine);

  std::cout << "Main: Starting scheduler..." << std::endl;
  scheduler.run();
  std::cout << "Main: Scheduler finished." << std::endl;

  return 0;
}
