#include "coroutines.hpp"
#include <chrono>
#include <deque>
#include <iostream>
#include <random>
#include <string>

const int NUM_PRODUCERS = 3;
const int NUM_CONSUMERS = 3;
const int BUFFER_CAPACITY = 5;
const int MAX_LOG_LINES = 5;

std::deque<std::string> log_messages;

// ANSI Escape Codes
#define CLEAR_SCREEN "\033[2J\033[H"
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE "\033[34m"

class Buffer {
public:
  Buffer(int capacity) : capacity_(capacity) {}

  bool is_full() const { return items_.size() >= capacity_; }
  bool is_empty() const { return items_.empty(); }
  int capacity() const { return capacity_; }
  const std::deque<int> &items() const { return items_; }

  void produce(int item) { items_.push_back(item); }
  int consume() {
    int item = items_.front();
    items_.pop_front();
    return item;
  }

private:
  std::deque<int> items_;
  int capacity_;
};

Buffer buffer(BUFFER_CAPACITY);

void log_message(const std::string &msg) {
  static const int LOG_START_ROW = 17;

  log_messages.push_back(msg);
  if (log_messages.size() > MAX_LOG_LINES) {
    log_messages.pop_front();
  }

  for (size_t i = 0; i < log_messages.size(); ++i) {
    std::cout << "\033[" << (LOG_START_ROW + i) << ";1H\033[K"
              << log_messages[i];
  }
  std::cout << std::flush;
}

void update_producer_status(int id, const std::string &status,
                            const std::string &color) {
  int row = 4 + id;
  std::cout << "\033[" << row << ";13H" << COLOR_RESET << "\033[K";
  std::cout << color << status << COLOR_RESET << std::flush;
}

void update_consumer_status(int id, const std::string &status,
                            const std::string &color) {
  int row = 9 + id;
  std::cout << "\033[" << row << ";15H" << COLOR_RESET << "\033[K";
  std::cout << color << status << COLOR_RESET << std::flush;
}

void update_buffer_display() {
  const int current_size = buffer.items().size();
  std::cout << "\033[13;1H" << COLOR_BLUE << "Buffer (" << current_size << "/"
            << buffer.capacity() << ") [";
  for (int i = 0; i < buffer.capacity(); ++i) {
    std::cout << (i < current_size ? "▓" : "░");
  }
  std::cout << "]" << COLOR_RESET << std::flush;
}

void initialize_display() {
  std::cout << CLEAR_SCREEN << "\033[1;1H" << COLOR_GREEN
            << "Produtores e Consumidores" << COLOR_RESET << "\033[3;1H"
            << COLOR_YELLOW << "Produtores:" << COLOR_RESET;

  for (int i = 0; i < NUM_PRODUCERS; ++i) {
    std::cout << "\033[" << (4 + i) << ";1HProdutor " << i << ": ";
  }

  std::cout << "\033[8;1H" << COLOR_YELLOW << "Consumidores:" << COLOR_RESET;
  for (int i = 0; i < NUM_CONSUMERS; ++i) {
    std::cout << "\033[" << (9 + i) << ";1HConsumidor " << i << ": ";
  }

  std::cout << "\033[16;1H" << COLOR_YELLOW
            << "Log de Atividades:" << COLOR_RESET;
  update_buffer_display();
  std::cout << std::flush;
}

void producer(Coroutine *coro, int id) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> work_time(500, 1500);
  std::uniform_int_distribution<> item_gen(1, 100);

  while (true) {
    int item = item_gen(gen);
    log_message("Produtor " + std::to_string(id) + " gerou " +
                std::to_string(item));

    while (buffer.is_full()) {
      update_producer_status(id, "Esperando (cheio)", COLOR_RED);
      coro->yield();
    }

    buffer.produce(item);
    update_producer_status(id, "Produzindo...", COLOR_GREEN);
    log_message("Produtor " + std::to_string(id) + " adicionou " +
                std::to_string(item));
    update_buffer_display();

    coro->sleep_for(std::chrono::milliseconds(work_time(gen)));
    coro->yield();
  }
}

void consumer(Coroutine *coro, int id) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> work_time(500, 1500);

  while (true) {
    while (buffer.is_empty()) {
      update_consumer_status(id, "Esperando (vazio)", COLOR_RED);
      coro->yield();
    }

    int item = buffer.consume();
    update_consumer_status(id, "Consumindo... ", COLOR_GREEN);
    log_message("Consumidor " + std::to_string(id) + " removeu " +
                std::to_string(item));
    update_buffer_display();

    coro->sleep_for(std::chrono::milliseconds(work_time(gen)));
    coro->yield();
  }
}

int main() {
  initialize_display();
  Scheduler scheduler;

  for (int i = 0; i < NUM_PRODUCERS; ++i) {
    scheduler.add_coroutine([i](Coroutine *coro) { producer(coro, i); });
  }

  for (int i = 0; i < NUM_CONSUMERS; ++i) {
    scheduler.add_coroutine([i](Coroutine *coro) { consumer(coro, i); });
  }

  scheduler.run();
  return 0;
}
