#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

const int NUM_PHILOSOPHERS = 5;
const int MAX_LOG_LINES = 5;

// ANSI Escape Codes
#define CLEAR_SCREEN "\033[2J\033[H"
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"

enum class Status { THINKING, WAITING, EATING };

struct SystemState {
  std::array<Status, NUM_PHILOSOPHERS> philosopher_status;
  std::array<bool, NUM_PHILOSOPHERS> fork_status;
  std::deque<std::string> log_messages;
};

SystemState g_state;
std::mutex g_state_mutex;

std::condition_variable g_ui_update_cv;
bool g_ui_update_needed = false;
std::mutex g_ui_cv_mutex;

void signal_ui_update() {
  {
    std::lock_guard<std::mutex> lock(g_ui_cv_mutex);
    g_ui_update_needed = true;
  }
  g_ui_update_cv.notify_one();
}

std::array<std::mutex, NUM_PHILOSOPHERS> forks;

void update_philosopher_status(int id, Status new_status) {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state.philosopher_status[id] = new_status;
  }
  signal_ui_update();
}

void update_fork_status(int fork_id, bool is_taken) {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state.fork_status[fork_id] = is_taken;
  }
  signal_ui_update();
}

void add_log_message(const std::string &msg) {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state.log_messages.push_back(msg);
    if (g_state.log_messages.size() > MAX_LOG_LINES) {
      g_state.log_messages.pop_front();
    }
  }
  signal_ui_update();
}

void ui_thread_function() {
  SystemState local_state;

  std::cout << CLEAR_SCREEN << "\033[1;1H" << COLOR_GREEN
            << "Jantar dos Filosofos" << COLOR_YELLOW
            << "\nFilosofo Status:" << COLOR_RESET << "\033[3;1HFilosofo 0:"
            << "\033[4;1HFilosofo 1:"
            << "\033[5;1HFilosofo 2:"
            << "\033[6;1HFilosofo 3:"
            << "\033[7;1HFilosofo 4:" << std::flush;

  while (true) {
    {
      std::unique_lock<std::mutex> cv_lock(g_ui_cv_mutex);
      g_ui_update_cv.wait(cv_lock, [] { return g_ui_update_needed; });
      g_ui_update_needed = false;
    }

    {
      std::lock_guard<std::mutex> state_lock(g_state_mutex);
      local_state = g_state;
    }

    for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
      int row = 3 + i;
      std::string status_text;
      std::string color;

      switch (local_state.philosopher_status[i]) {
      case Status::THINKING:
        status_text = "Pensando...";
        color = COLOR_YELLOW;
        break;
      case Status::WAITING:
        status_text = "Aguardando...";
        color = COLOR_RED;
        break;
      case Status::EATING:
        status_text = "Comendo...  ";
        color = COLOR_GREEN;
        break;
      }

      std::cout << "\033[" << row << ";20H" << COLOR_RESET << "\033[K";
      std::cout << color << status_text << COLOR_RESET;
    }

    std::cout << "\033[9;1H" << COLOR_BLUE << "Garfos:  ";
    for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
      std::cout << i << ":" << (local_state.fork_status[i] ? "▓" : "░") << " ";
    }
    std::cout << COLOR_RESET;

    std::cout << "\033[10;1H" << COLOR_YELLOW
              << "Log de Atividades:" << COLOR_RESET;
    static const int LOG_START_ROW = 11;
    for (size_t i = 0; i < local_state.log_messages.size(); ++i) {
      std::cout << "\033[" << (LOG_START_ROW + i) << ";1H\033[K"
                << local_state.log_messages[i];
    }

    std::cout << std::flush;
  }
}

void philosopher(int id) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> think_time(2000, 5000);
  std::uniform_int_distribution<> eat_time(1000, 3000);

  int left_fork = id;
  int right_fork = (id + 1) % NUM_PHILOSOPHERS;

  // Previnir deadlock ultimo filosofo pega os garfos em ordem invertida
  if (id == NUM_PHILOSOPHERS - 1) {
    std::swap(left_fork, right_fork);
  }

  while (true) {
    update_philosopher_status(id, Status::THINKING);
    add_log_message("Filosofo " + std::to_string(id) + " esta pensando...");
    std::this_thread::sleep_for(std::chrono::milliseconds(think_time(gen)));

    update_philosopher_status(id, Status::WAITING);

    add_log_message("Filosofo " + std::to_string(id) +
                    " tentando pegar garfo " + std::to_string(left_fork));
    forks[left_fork].lock();
    update_fork_status(left_fork, true);

    add_log_message("Filosofo " + std::to_string(id) +
                    " tentando pegar garfo " + std::to_string(right_fork));
    forks[right_fork].lock();
    update_fork_status(right_fork, true);

    update_philosopher_status(id, Status::EATING);
    add_log_message("Filosofo " + std::to_string(id) + " adquiriu os garfos " +
                    std::to_string(left_fork) + " & " +
                    std::to_string(right_fork));
    std::this_thread::sleep_for(std::chrono::milliseconds(eat_time(gen)));

    forks[right_fork].unlock();
    update_fork_status(right_fork, false);

    forks[left_fork].unlock();
    update_fork_status(left_fork, false);

    add_log_message("Filosofo " + std::to_string(id) + " liberou os garfos " +
                    std::to_string(left_fork) + " & " +
                    std::to_string(right_fork));
  }
}

int main() {
  for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
    g_state.philosopher_status[i] = Status::THINKING;
  }

  std::thread ui_thread(ui_thread_function);
  ui_thread.detach(); // Detach UI thread to run independently

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::vector<std::thread> philosophers;
  for (int i = 0; i < NUM_PHILOSOPHERS; ++i) {
    philosophers.emplace_back(philosopher, i);
  }

  for (auto &t : philosophers) {
    t.join();
  }

  return 0;
}
