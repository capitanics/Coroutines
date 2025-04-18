#include "../src/co_lib.hpp"
#include <chrono>
#include <deque>
#include <iostream>
#include <random>
#include <string>

const int NUM_PHILOSOPHERS = 5;
std::deque<std::string> log_messages;
const int MAX_LOG_LINES = 5;

// ANSI Escape Codes
#define CLEAR_SCREEN "\033[2J\033[H"
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"

using namespace co_lib;

class Table {
public:
  Table() {
    for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
      forks_[i] = false;
    }
  }

  bool request_forks(int left_fork, int right_fork) {
    if (!forks_[left_fork]) {
      forks_[left_fork] = true;
      if (!forks_[right_fork]) {
        forks_[right_fork] = true;
        return true;
      } else {
        forks_[left_fork] = false;
      }
    }
    return false;
  }

  void release_forks(int left_fork, int right_fork) {
    forks_[left_fork] = false;
    forks_[right_fork] = false;
  }

  bool get_fork_state(int index) const { return forks_[index]; }

private:
  bool forks_[NUM_PHILOSOPHERS];
};

Table table;

void update_philosopher_status(int id, const std::string &status,
                               const std::string &color) {
  int row = 3 + id; // Philosophers start at row 3
  std::cout << "\033[" << row << ";20H" << COLOR_RESET << "\033[K";
  std::cout << color << status << COLOR_RESET << std::flush;
}

void update_forks_display() {
  std::cout << "\033[9;1H" << COLOR_BLUE << "Garfos:  ";
  for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
    std::cout << i << ":" << (table.get_fork_state(i) ? "▓" : "░") << " ";
  }
  std::cout << COLOR_RESET << std::flush;
}

void log_message(const std::string &msg) {
  static const int LOG_START_ROW = 11;

  log_messages.push_back(msg);
  if (log_messages.size() > MAX_LOG_LINES) {
    log_messages.pop_front();
  }

  std::cout << "\033[10;1H" << COLOR_YELLOW
            << "Log de  Atividades:" << COLOR_RESET;
  for (size_t i = 0; i < log_messages.size(); ++i) {
    std::cout << "\033[" << (LOG_START_ROW + i) << ";1H\033[K"
              << log_messages[i];
  }
  std::cout << std::flush;
}

void initialize_display() {
  std::cout << CLEAR_SCREEN << "\033[1;1H" << COLOR_GREEN
            << "Jantar dos Filosofos" << COLOR_YELLOW
            << "\nFilosofo Status:" << COLOR_RESET << "\033[3;1HFilosofo 0:"
            << "\033[4;1HFilosofo 1:"
            << "\033[5;1HFilosofo 2:"
            << "\033[6;1HFilosofo 3:"
            << "\033[7;1HFilosofo 4:";
  update_forks_display();
  std::cout << std::flush;
}

void philosopher(Coroutine *coro, int id) {
  int left_fork = id;
  int right_fork = (id + 1) % NUM_PHILOSOPHERS;

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> think_time(2000, 5000);
  std::uniform_int_distribution<> eat_time(1000, 3000);

  while (true) {
    update_philosopher_status(id, "Pensando...", COLOR_YELLOW);
    log_message("Filosofo " + std::to_string(id) + " esta pensando...");
    coro->sleep_for(std::chrono::milliseconds(think_time(gen)));

    update_philosopher_status(id, "Aguardando...", COLOR_RED);
    while (!table.request_forks(left_fork, right_fork)) {
      coro->yield();
    }
    update_forks_display();

    update_philosopher_status(id, "Comendo...  ", COLOR_GREEN);
    log_message("Filosofo " + std::to_string(id) + " adquiriu os garfos " +
                std::to_string(left_fork) + " & " + std::to_string(right_fork));
    coro->sleep_for(std::chrono::milliseconds(eat_time(gen)));

    table.release_forks(left_fork, right_fork);
    update_forks_display();
    log_message("Filosofo " + std::to_string(id) + " liberou os garfos " +
                std::to_string(left_fork) + " & " + std::to_string(right_fork));
    coro->yield();
  }
}

int main() {
  initialize_display();
  Scheduler scheduler;

  for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
    scheduler.add_coroutine([i](Coroutine *coro) { philosopher(coro, i); });
  }
  scheduler.run();
  return 0;
}
