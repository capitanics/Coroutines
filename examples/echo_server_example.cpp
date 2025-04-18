#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/co_lib.hpp"

const int SERVER_PORT = 9090;
const int MAX_CONNECTIONS = 1024;
const size_t IO_BUFFER_SIZE = 4096;

void die(const std::string &msg) {
  perror(msg.c_str());
  exit(EXIT_FAILURE);
}

bool set_non_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    perror("fcntl F_GETFL");
    return false;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("fcntl F_SETFL O_NONBLOCK");
    return false;
  }
  return true;
}

void echo_handler(co_lib::Coroutine *self, int client_fd) {
  std::cout << "[Echo Coro " << self << "] Handling connection fd=" << client_fd
            << std::endl;

  std::vector<char> read_buffer(IO_BUFFER_SIZE);
  std::deque<char> write_buffer;
  bool should_close = false;

  try {
    while (!should_close) {

      bool read_blocked = false;
      bool write_blocked = false;

      if (!write_buffer.empty()) {
        std::vector<char> temp_send_buf(write_buffer.begin(),
                                        write_buffer.end());

        ssize_t bytes_sent = send(client_fd, temp_send_buf.data(),
                                  temp_send_buf.size(), MSG_NOSIGNAL);

        if (bytes_sent > 0) {
          write_buffer.erase(write_buffer.begin(),
                             write_buffer.begin() + bytes_sent);
        } else if (bytes_sent == 0) {
          std::cerr << "[Echo Coro " << self
                    << "] send returned 0 unexpectedly for fd=" << client_fd
                    << ". Closing." << std::endl;
          should_close = true;
        } else {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            write_blocked = true;
          } else {
            perror("send error");
            should_close = true;
          }
        }
      }
      if (!should_close && write_buffer.empty()) {
        ssize_t bytes_read =
            recv(client_fd, read_buffer.data(), read_buffer.size(), 0);

        if (bytes_read > 0) {
          write_buffer.insert(write_buffer.end(), read_buffer.data(),
                              read_buffer.data() + bytes_read);
        } else if (bytes_read == 0) {
          std::cout << "[Echo Coro " << self << "] Client fd=" << client_fd
                    << " disconnected." << std::endl;
          should_close = true;
        } else {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            read_blocked = true;
          } else {
            perror("recv error");
            should_close = true;
          }
        }
      }

      if (read_blocked && (write_buffer.empty() || write_blocked)) {
        self->await_read(client_fd);
      } else if (write_blocked && !write_buffer.empty()) {
        self->await_write(client_fd);
      } else if (!should_close && write_buffer.empty() && !read_blocked) {
        self->await_read(client_fd);
      }
    }

  } catch (const co_lib::CoroutineCancelledException &e) {
    std::cerr << "[Echo Coro " << self
              << "] Cancelled while handling fd=" << client_fd << ": "
              << e.what() << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[Echo Coro " << self
              << "] Exception while handling fd=" << client_fd << ": "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Echo Coro " << self
              << "] Unknown exception while handling fd=" << client_fd
              << std::endl;
  }

  std::cout << "[Echo Coro " << self << "] Closing connection fd=" << client_fd
            << std::endl;
  close(client_fd);
}

void server_loop(co_lib::Coroutine *self, co_lib::Scheduler *scheduler,
                 int port) {
  int listen_fd = -1;

  try {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "socket creation failed");
    }

    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
      perror("Warning: setsockopt(SO_REUSEADDR) failed");
    }

    if (!set_non_blocking(listen_fd)) {
      throw std::runtime_error("Failed to set listening socket non-blocking");
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
        0) {
      throw std::system_error(errno, std::generic_category(),
                              "bind failed on port " + std::to_string(port));
    }

    if (listen(listen_fd, MAX_CONNECTIONS) < 0) {
      throw std::system_error(errno, std::generic_category(), "listen failed");
    }

    std::cout << "[Server Coro] Echo server listening on port " << port
              << " (fd=" << listen_fd << ")" << std::endl;

    while (true) {

      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);
      int client_fd =
          accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

      if (client_fd >= 0) {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[Server Coro] Accepted connection from " << client_ip
                  << ":" << ntohs(client_addr.sin_port) << " (fd=" << client_fd
                  << ")" << std::endl;

        if (!set_non_blocking(client_fd)) {
          std::cerr << "Warning: Failed to set client socket non-blocking. "
                       "Closing fd="
                    << client_fd << std::endl;
          close(client_fd);
          continue;
        }

        scheduler->add_coroutine([client_fd](co_lib::Coroutine *handler_self) {
          echo_handler(handler_self, client_fd);
        }

        );

      } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          self->await_read(listen_fd); // Yield until listen_fd is readable
        } else {
          perror("accept error");
          self->sleep_for(std::chrono::milliseconds(100));
        }
      }
    }

  } catch (const co_lib::CoroutineCancelledException &e) {
    std::cerr << "[Server Coro] Cancelled: " << e.what() << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[Server Coro] Error: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Server Coro] Unknown error occurred." << std::endl;
  }

  if (listen_fd >= 0) {
    std::cout << "[Server Coro] Shutting down listener fd=" << listen_fd
              << std::endl;
    close(listen_fd);
  }
}

int main() {
  try {
    co_lib::Scheduler scheduler;

    std::cout << "Starting echo server coroutine..." << std::endl;
    co_lib::Coroutine *server_coro =
        scheduler.add_coroutine([&scheduler](co_lib::Coroutine *self) {
          server_loop(self, &scheduler, SERVER_PORT);
        });

    if (!server_coro) {
      std::cerr << "Fatal: Failed to create server coroutine." << std::endl;
      return 1;
    }

    std::cout << "Running scheduler..." << std::endl;
    scheduler.run();

    std::cout << "Scheduler finished." << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "Unhandled exception in main: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Unknown unhandled exception in main." << std::endl;
    return 1;
  }

  return 0;
}
