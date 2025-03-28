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

using namespace std;

const int NUM_PRODUCERS = 3;
const int NUM_CONSUMERS = 3;
const int BUFFER_CAPACITY = 5;
const int MAX_LOG_LINES = 5;

// ANSI Escape Codes
#define CLEAR_SCREEN "\033[2J\033[H"
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"

enum class Status { PRODUCING, WAITING, CONSUMING };

struct SystemState {
  array<Status, NUM_PRODUCERS> producer_status;
  array<Status, NUM_CONSUMERS> consumer_status;
  int buffer_size = 0;
  deque<string> log_messages;
};

SystemState g_state;
mutex g_state_mutex;
condition_variable g_ui_update_cv;
bool g_ui_update_needed = false;
mutex g_ui_cv_mutex;

// Buffer synchronization
mutex buffer_mutex;
condition_variable buffer_cond;
int buffer_size = 0;

void signal_ui_update() {
  {
    lock_guard<mutex> lock(g_ui_cv_mutex);
    g_ui_update_needed = true;
  }
  g_ui_update_cv.notify_one();
}

void update_producer_status(int id, Status new_status) {
  lock_guard<mutex> lock(g_state_mutex);
  g_state.producer_status[id] = new_status;
  signal_ui_update();
}

void update_consumer_status(int id, Status new_status) {
  lock_guard<mutex> lock(g_state_mutex);
  g_state.consumer_status[id] = new_status;
  signal_ui_update();
}

void add_log_message(const string &msg) {
  {
    lock_guard<mutex> lock(g_state_mutex);
    g_state.log_messages.push_back(msg);
    if (g_state.log_messages.size() > MAX_LOG_LINES) {
      g_state.log_messages.pop_front();
    }
  }
  signal_ui_update();
}

void ui_thread_function() {
  SystemState local_state;
  cout << CLEAR_SCREEN << "\033[1;1H" << COLOR_GREEN
       << "Produtores e Consumidores" << COLOR_RESET << flush;

  while (true) {
    {
      unique_lock<mutex> cv_lock(g_ui_cv_mutex);
      g_ui_update_cv.wait(cv_lock, [] { return g_ui_update_needed; });
      g_ui_update_needed = false;
    }

    {
      lock_guard<mutex> state_lock(g_state_mutex);
      local_state = g_state;
    }

    // Update producers
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
      int row = 3 + i;
      string status;
      string color;
      switch (local_state.producer_status[i]) {
      case Status::PRODUCING:
        status = "Produzindo...";
        color = COLOR_GREEN;
        break;
      case Status::WAITING:
        status = "Aguardando...";
        color = COLOR_RED;
        break;
      default:
        break;
      }
      cout << "\033[" << row << ";1H" << COLOR_RESET << "\033[K"
           << "Produtor " << i << ": " << color << status << COLOR_RESET;
    }

    // Update buffer
    cout << "\033[7;1H" << COLOR_BLUE << "Buffer [";
    int filled = local_state.buffer_size;
    for (int i = 0; i < BUFFER_CAPACITY; ++i) {
      cout << (i < filled ? "▓" : "░");
    }
    cout << "]" << COLOR_RESET << "\033[K";

    // Update consumers
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
      int row = 9 + i;
      string status;
      string color;
      switch (local_state.consumer_status[i]) {
      case Status::CONSUMING:
        status = "Consumindo...";
        color = COLOR_GREEN;
        break;
      case Status::WAITING:
        status = "Aguardando...";
        color = COLOR_RED;
        break;
      default:
        break;
      }
      cout << "\033[" << row << ";1H" << COLOR_RESET << "\033[K"
           << "Consumidor " << i << ": " << color << status << COLOR_RESET;
    }

    // Update logs
    cout << "\033[13;1H" << COLOR_YELLOW << "Logs:" << COLOR_RESET;
    for (size_t i = 0; i < local_state.log_messages.size(); ++i) {
      cout << "\033[" << (14 + i) << ";1H\033[K" << local_state.log_messages[i];
    }

    cout << flush;
  }
}

void producer(int id) {
  random_device rd;
  mt19937 gen(rd());
  uniform_int_distribution<> produce_time(1000, 3000);

  while (true) {
    update_producer_status(id, Status::PRODUCING);
    add_log_message("Produtor " + to_string(id) + " produzindo...");
    this_thread::sleep_for(chrono::milliseconds(produce_time(gen)));

    unique_lock<mutex> lock(buffer_mutex);
    while (buffer_size >= BUFFER_CAPACITY) {
      update_producer_status(id, Status::WAITING);
      add_log_message("Produtor " + to_string(id) + " esperando...");
      buffer_cond.wait(lock);
    }

    buffer_size++;
    {
      lock_guard<mutex> state_lock(g_state_mutex);
      g_state.buffer_size = buffer_size;
    }
    add_log_message("Produtor " + to_string(id) +
                    " adicionou. Buffer: " + to_string(buffer_size));
    lock.unlock();
    buffer_cond.notify_all();
  }
}

void consumer(int id) {
  random_device rd;
  mt19937 gen(rd());
  uniform_int_distribution<> consume_time(1500, 4000);

  while (true) {
    update_consumer_status(id, Status::CONSUMING);
    add_log_message("Consumidor " + to_string(id) + " pronto...");
    this_thread::sleep_for(chrono::milliseconds(consume_time(gen)));

    unique_lock<mutex> lock(buffer_mutex);
    while (buffer_size == 0) {
      update_consumer_status(id, Status::WAITING);
      add_log_message("Consumidor " + to_string(id) + " esperando...");
      buffer_cond.wait(lock);
    }

    buffer_size--;
    {
      lock_guard<mutex> state_lock(g_state_mutex);
      g_state.buffer_size = buffer_size;
    }
    add_log_message("Consumidor " + to_string(id) +
                    " consumiu. Buffer: " + to_string(buffer_size));
    lock.unlock();
    buffer_cond.notify_all();
  }
}

int main() {
  for (int i = 0; i < NUM_PRODUCERS; ++i) {
    g_state.producer_status[i] = Status::PRODUCING;
  }
  for (int i = 0; i < NUM_CONSUMERS; ++i) {
    g_state.consumer_status[i] = Status::CONSUMING;
  }

  thread ui_thread(ui_thread_function);
  ui_thread.detach();
  this_thread::sleep_for(chrono::milliseconds(100));

  vector<thread> producers;
  for (int i = 0; i < NUM_PRODUCERS; ++i) {
    producers.emplace_back(producer, i);
  }

  vector<thread> consumers;
  for (int i = 0; i < NUM_CONSUMERS; ++i) {
    consumers.emplace_back(consumer, i);
  }

  for (auto &t : producers)
    t.join();
  for (auto &t : consumers)
    t.join();

  return 0;
}
