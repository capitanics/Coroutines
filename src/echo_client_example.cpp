#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coroutines.hpp"

const char *SERVER_HOST = "127.0.0.1";
const int SERVER_PORT = 9090;
const size_t IO_BUFFER_SIZE = 1024;

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

void echo_client_coro(co_lib::Coroutine *self, const std::string &host,
                      int port) {
  int sock_fd = -1;
  std::cout << "[Client Coro] Starting. Connecting to " << host << ":" << port
            << std::endl;

  try {
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "socket creation failed");
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
      throw std::runtime_error("Invalid server address format");
    }

    if (!set_non_blocking(sock_fd)) {
      throw std::runtime_error("Failed to set client socket non-blocking");
    }

    int connect_ret =
        connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (connect_ret < 0) {
      if (errno == EINPROGRESS) {
        std::cout << "[Client Coro] Connection in progress, awaiting writable "
                     "socket..."
                  << std::endl;
        self->await_write(sock_fd);

        int sock_err = 0;
        socklen_t err_len = sizeof(sock_err);
        if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len) <
            0) {
          perror("getsockopt(SO_ERROR) failed");
          throw std::system_error(errno, std::generic_category(),
                                  "getsockopt(SO_ERROR) failed");
        }
        if (sock_err != 0) {
          throw std::system_error(sock_err, std::generic_category(),
                                  "connect failed after wait");
        }
        std::cout << "[Client Coro] Connection established (fd=" << sock_fd
                  << ")" << std::endl;
      } else {
        throw std::system_error(errno, std::generic_category(),
                                "connect failed immediately");
      }
    } else {
      std::cout << "[Client Coro] Connection established immediately (fd="
                << sock_fd << ")" << std::endl;
    }

    if (!set_non_blocking(STDIN_FILENO)) {
      throw std::runtime_error("Failed to set stdin non-blocking");
    }
    std::cout
        << "[Client Coro] Please type messages to send (or empty line to quit):"
        << std::endl;

    std::vector<char> read_buf(IO_BUFFER_SIZE);
    std::string line_buffer;
    std::string send_buffer;
    std::string recv_buffer;

    bool stdin_eof = false;

    while (true) {

      if (!stdin_eof && send_buffer.empty()) {
        ssize_t stdin_read =
            read(STDIN_FILENO, read_buf.data(), read_buf.size());
        if (stdin_read > 0) {
          line_buffer.append(read_buf.data(), stdin_read);
          size_t newline_pos;
          while ((newline_pos = line_buffer.find('\n')) != std::string::npos) {
            send_buffer = line_buffer.substr(0, newline_pos + 1);
            line_buffer.erase(0, newline_pos + 1);

            if (send_buffer == "\n") {
              std::cout << "[Client Coro] Empty line entered, quitting."
                        << std::endl;
              stdin_eof = true;
              break;
            }
            std::cout << "[Client Coro] Read from stdin: " << send_buffer;
            break;
          }
        } else if (stdin_read == 0) {
          std::cout << "[Client Coro] Stdin EOF reached." << std::endl;
          stdin_eof = true;
        } else {
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("read stdin error");
            stdin_eof = true;
          }
        }
      }

      if (!send_buffer.empty()) {
        ssize_t bytes_sent =
            send(sock_fd, send_buffer.data(), send_buffer.size(), MSG_NOSIGNAL);
        if (bytes_sent > 0) {
          send_buffer.erase(0, bytes_sent);
          if (!send_buffer.empty()) {

          } else {
          }
        } else if (bytes_sent == 0) {
          std::cerr << "[Client Coro] send returned 0 unexpectedly."
                    << std::endl;
          break;
        } else {
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("send error");
            break;
          }
        }
      }

      ssize_t bytes_recv = recv(sock_fd, read_buf.data(), read_buf.size(), 0);
      if (bytes_recv > 0) {

        std::cout << "[Server Echo] ";
        std::cout.write(read_buf.data(), bytes_recv);

      } else if (bytes_recv == 0) {
        std::cout << "[Client Coro] Server disconnected fd=" << sock_fd
                  << std::endl;
        break;
      } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          perror("recv error");
          break;
        }
      }

      bool need_read_stdin = !stdin_eof && send_buffer.empty();
      bool need_write_sock = !send_buffer.empty();
      bool need_read_sock = true;

      int stdin_ready = 0;
      int sock_ready_read = 0;
      int sock_ready_write = 0;

      if ((need_read_stdin && errno == EAGAIN) ||
          (need_write_sock && errno == EAGAIN) ||
          (need_read_sock && errno == EAGAIN)) {
        if (need_read_stdin && errno == EAGAIN) {

          self->await_read(STDIN_FILENO);
        } else if (need_write_sock && errno == EAGAIN) {

          self->await_write(sock_fd);
        } else if (need_read_sock && errno == EAGAIN) {

          self->await_read(sock_fd);
        }

      } else if (stdin_eof && send_buffer.empty()) {

        if (errno == EAGAIN) {

          self->await_read(sock_fd);
        } else {
        }
      }

      if (stdin_eof && send_buffer.empty() && errno != EAGAIN) {
      }
    }

  } catch (const co_lib::CoroutineCancelledException &e) {
    std::cerr << "[Client Coro] Cancelled: " << e.what() << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[Client Coro] Error: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Client Coro] Unknown error occurred." << std::endl;
  }

  if (sock_fd >= 0) {
    std::cout << "[Client Coro] Closing socket fd=" << sock_fd << std::endl;
    close(sock_fd);
  }
  std::cout << "[Client Coro] Finished." << std::endl;
}

int main(int argc, char *argv[]) {
  std::string host = SERVER_HOST;
  int port = SERVER_PORT;

  if (argc > 1)
    host = argv[1];
  if (argc > 2)
    port = std::stoi(argv[2]);

  try {
    co_lib::Scheduler scheduler;

    std::cout << "Starting echo client coroutine..." << std::endl;
    co_lib::Coroutine *client_coro = scheduler.add_coroutine(

        [host, port](co_lib::Coroutine *self) {
          echo_client_coro(self, host, port);
        });

    if (!client_coro) {
      std::cerr << "Fatal: Failed to create client coroutine." << std::endl;
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
