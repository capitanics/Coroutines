#include <cstring>
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

const int SERVER_PORT = 8080;
const int MAX_CONNECTIONS = 1024;
const size_t READ_BUFFER_SIZE = 1024;

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

void handle_connection(co_lib::Coroutine *self, int client_fd) {
  std::cout << "[Coroutine " << self << "] Handling connection fd=" << client_fd
            << std::endl;
  std::vector<char> buffer(READ_BUFFER_SIZE);
  std::string request_data;
  bool keep_reading = true;

  try {
    while (keep_reading) {
      ssize_t bytes_read = recv(client_fd, buffer.data(), buffer.size(), 0);

      if (bytes_read > 0) {
        request_data.append(buffer.data(), bytes_read);
        if (request_data.find("\r\n\r\n") != std::string::npos) {
          keep_reading = false;
        }
      } else if (bytes_read == 0) {
        std::cout << "[Coroutine " << self << "] Client fd=" << client_fd
                  << " disconnected." << std::endl;
        keep_reading = false;
      } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          self->await_read(client_fd); // Yield until readable
        } else {
          perror("recv error");
          keep_reading = false;
        }
      }
    }

    if (request_data.empty()) {
      std::cout << "[Coroutine " << self
                << "] No request data received from fd=" << client_fd
                << std::endl;
    } else {
      std::string response_body = "Hello, World!";
      std::string http_response = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/plain\r\n"
                                  "Content-Length: " +
                                  std::to_string(response_body.length()) +
                                  "\r\n"
                                  "Connection: close\r\n"
                                  "\r\n" +
                                  response_body;

      size_t total_sent = 0;
      while (total_sent < http_response.length()) {
        ssize_t bytes_sent =
            send(client_fd, http_response.data() + total_sent,
                 http_response.length() - total_sent, MSG_NOSIGNAL);

        if (bytes_sent > 0) {
          total_sent += bytes_sent;
        } else if (bytes_sent == 0) {
          std::cerr << "[Coroutine " << self
                    << "] send returned 0 unexpectedly for fd=" << client_fd
                    << std::endl;
          break;
        } else {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            self->await_write(client_fd);
          } else {
            perror("send error");
            break;
          }
        }
      }
      std::cout << "[Coroutine " << self
                << "] Sent response to fd=" << client_fd << " (" << total_sent
                << " bytes)" << std::endl;
    }

  } catch (const co_lib::CoroutineCancelledException &e) {
    std::cerr << "[Coroutine " << self
              << "] Cancelled while handling fd=" << client_fd << ": "
              << e.what() << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[Coroutine " << self
              << "] Exception while handling fd=" << client_fd << ": "
              << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Coroutine " << self
              << "] Unknown exception while handling fd=" << client_fd
              << std::endl;
  }

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
      throw std::system_error(errno, std::generic_category(),
                              "setsockopt SO_REUSEADDR failed");
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
      throw std::system_error(errno, std::generic_category(), "bind failed");
    }

    if (listen(listen_fd, MAX_CONNECTIONS) < 0) {
      throw std::system_error(errno, std::generic_category(), "listen failed");
    }

    std::cout << "[Server Coroutine] Listening on port " << port
              << " (fd=" << listen_fd << ")" << std::endl;

    while (true) {

      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);
      int client_fd =
          accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

      if (client_fd >= 0) {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[Server Coroutine] Accepted connection from " << client_ip
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
          handle_connection(handler_self, client_fd);
        });

      } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          self->await_read(listen_fd);
        } else {
          perror("accept error");
          self->sleep_for(std::chrono::milliseconds(100));
        }
      }
    }

  } catch (const co_lib::CoroutineCancelledException &e) {
    std::cerr << "[Server Coroutine] Cancelled: " << e.what() << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[Server Coroutine] Error: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Server Coroutine] Unknown error occurred." << std::endl;
  }

  if (listen_fd >= 0) {
    std::cout << "[Server Coroutine] Shutting down listener fd=" << listen_fd
              << std::endl;
    close(listen_fd);
  }
}

int main() {
  try {
    co_lib::Scheduler scheduler;

    std::cout << "Starting server coroutine..." << std::endl;
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
