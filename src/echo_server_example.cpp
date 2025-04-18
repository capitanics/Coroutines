
#include <cstring> // For strerror, memset
#include <deque>   // Using deque for efficient front removal in buffer
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error> // For std::system_error
#include <vector>

// Networking headers
#include <arpa/inet.h>
#include <fcntl.h> // For fcntl
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h> // For close

// Include your coroutine library header
#include "coroutines.hpp" // Assuming it's in the include path

// --- Configuration ---
const int SERVER_PORT = 9090;       // Port for the echo server
const int MAX_CONNECTIONS = 1024;   // Max pending connections for listen()
const size_t IO_BUFFER_SIZE = 4096; // Buffer size for reading data

// --- Error Handling Helper ---
// Consider putting this in a common utility header
void die(const std::string &msg) {
  perror(msg.c_str());
  exit(EXIT_FAILURE);
}

// --- Set Socket Non-Blocking ---
// Consider putting this in a common utility header
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

// --- Echo Handler Coroutine ---
// This function runs in its own coroutine for each client connection.
// It reads data and immediately tries to write it back (echo).
void echo_handler(co_lib::Coroutine *self, int client_fd) {
  std::cout << "[Echo Coro " << self << "] Handling connection fd=" << client_fd
            << std::endl;

  std::vector<char> read_buffer(IO_BUFFER_SIZE);
  // Use deque for efficient removal from front after sending
  std::deque<char> write_buffer;
  bool should_close = false;

  try {
    while (!should_close) {

      bool read_blocked = false;
      bool write_blocked = false;

      // --- 1. Try to write pending data first ---
      if (!write_buffer.empty()) {
        // Convert deque segment to contiguous buffer for send (inefficient for
        // large buffers) A more optimized approach might use scatter/gather I/O
        // (writev) or a ring buffer.
        std::vector<char> temp_send_buf(write_buffer.begin(),
                                        write_buffer.end());

        ssize_t bytes_sent = send(client_fd, temp_send_buf.data(),
                                  temp_send_buf.size(), MSG_NOSIGNAL);

        if (bytes_sent > 0) {
          // Remove sent data from the front of the deque
          write_buffer.erase(write_buffer.begin(),
                             write_buffer.begin() + bytes_sent);
          // std::cout << "[Echo Coro " << self << "] Sent " << bytes_sent << "
          // bytes to fd=" << client_fd << std::endl;
        } else if (bytes_sent == 0) {
          std::cerr << "[Echo Coro " << self
                    << "] send returned 0 unexpectedly for fd=" << client_fd
                    << ". Closing." << std::endl;
          should_close = true;
        } else { // bytes_sent < 0
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Cannot write right now, need to wait
            write_blocked = true;
          } else {
            perror("send error");
            should_close = true; // Close on send error
          }
        }
      }

      // --- 2. Try to read new data if we are not blocked on writing ---
      //    Only read if write buffer is empty to prevent it growing
      //    indefinitely if client sends faster than we can echo.
      if (!should_close && write_buffer.empty()) {
        ssize_t bytes_read =
            recv(client_fd, read_buffer.data(), read_buffer.size(), 0);

        if (bytes_read > 0) {
          // Add received data to the write buffer
          write_buffer.insert(write_buffer.end(), read_buffer.data(),
                              read_buffer.data() + bytes_read);
          // std::cout << "[Echo Coro " << self << "] Read " << bytes_read << "
          // bytes from fd=" << client_fd << std::endl;
        } else if (bytes_read == 0) {
          // Client disconnected gracefully
          std::cout << "[Echo Coro " << self << "] Client fd=" << client_fd
                    << " disconnected." << std::endl;
          should_close = true; // Stop loop, connection closed
        } else {               // bytes_read < 0
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available to read right now
            read_blocked = true;
          } else {
            // Actual recv error
            perror("recv error");
            should_close = true; // Close on receive error
          }
        }
      }

      // --- 3. Yield if necessary ---
      // If both read and write would block, or if one would block and the other
      // is idle.
      if (read_blocked && (write_buffer.empty() || write_blocked)) {
        // Need to read, but couldn't. Write buffer is either empty or also
        // blocked. Wait for readability. std::cout << "[Echo Coro " << self <<
        // "] Read blocked, awaiting read on fd=" << client_fd << std::endl;
        self->await_read(client_fd);
      } else if (write_blocked && !write_buffer.empty()) {
        // Need to write, but couldn't.
        // Wait for writability.
        // std::cout << "[Echo Coro " << self << "] Write blocked, awaiting
        // write on fd=" << client_fd << std::endl;
        self->await_write(client_fd);
      } else if (!should_close && write_buffer.empty() && !read_blocked) {
        // If write buffer is empty and read didn't block (meaning it read 0 or
        // error?), we might need to wait for reading again if not closing. This
        // case is tricky, often means client isn't sending anything. Let's wait
        // for read if nothing else is happening. std::cout << "[Echo Coro " <<
        // self << "] Idle, awaiting read on fd=" << client_fd << std::endl;
        self->await_read(client_fd);
      }
      // If neither read nor write blocked, loop continues immediately.

    } // end while(!should_close)

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

  // --- Cleanup ---
  std::cout << "[Echo Coro " << self << "] Closing connection fd=" << client_fd
            << std::endl;
  close(client_fd);
  // Coroutine finishes here
}

// --- Server Accept Loop Coroutine ---
// This coroutine listens for and accepts new connections.
// (Identical to the web server version, just launches echo_handler)
void server_loop(co_lib::Coroutine *self, co_lib::Scheduler *scheduler,
                 int port) {
  int listen_fd = -1;

  try {
    // 1. Create listening socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "socket creation failed");
    }

    // 2. Set reuse address option
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
      // Don't die on setsockopt failure, just warn
      perror("Warning: setsockopt(SO_REUSEADDR) failed");
    }

    // 3. Set listening socket non-blocking
    if (!set_non_blocking(listen_fd)) {
      throw std::runtime_error("Failed to set listening socket non-blocking");
    }

    // 4. Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
    server_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
        0) {
      throw std::system_error(errno, std::generic_category(),
                              "bind failed on port " + std::to_string(port));
    }

    // 5. Listen for connections
    if (listen(listen_fd, MAX_CONNECTIONS) < 0) {
      throw std::system_error(errno, std::generic_category(), "listen failed");
    }

    std::cout << "[Server Coro] Echo server listening on port " << port
              << " (fd=" << listen_fd << ")" << std::endl;

    // 6. Accept loop
    while (true) {
      // Cancellation check happens implicitly in await_read below

      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);
      int client_fd =
          accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

      if (client_fd >= 0) {
        // Connection accepted!
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[Server Coro] Accepted connection from " << client_ip
                  << ":" << ntohs(client_addr.sin_port) << " (fd=" << client_fd
                  << ")" << std::endl;

        // Set client socket non-blocking
        if (!set_non_blocking(client_fd)) {
          std::cerr << "Warning: Failed to set client socket non-blocking. "
                       "Closing fd="
                    << client_fd << std::endl;
          close(client_fd);
          continue; // Skip this connection
        }

        // Launch the ECHO handler coroutine for this client
        scheduler->add_coroutine([client_fd](co_lib::Coroutine *handler_self) {
          echo_handler(handler_self, client_fd);
        }
                                 // Optional: specify stack size for handler
                                 // coroutines , 64 * 1024 // e.g., 64KB stack
        );

      } else { // accept failed
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // No pending connections, wait for readability on listening socket
          // std::cout << "[Server Coro] accept would block, awaiting read on
          // fd=" << listen_fd << std::endl;
          self->await_read(listen_fd); // Yield until listen_fd is readable
          // std::cout << "[Server Coro] listen fd=" << listen_fd << " became
          // readable." << std::endl;
        } else {
          // Actual accept error (e.g., too many open files)
          perror("accept error");
          // Avoid busy-loop on persistent error
          self->sleep_for(std::chrono::milliseconds(100));
        }
      }
    } // end while(true) accept loop

  } catch (const co_lib::CoroutineCancelledException &e) {
    std::cerr << "[Server Coro] Cancelled: " << e.what() << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[Server Coro] Error: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[Server Coro] Unknown error occurred." << std::endl;
  }

  // Cleanup listening socket if it was opened
  if (listen_fd >= 0) {
    std::cout << "[Server Coro] Shutting down listener fd=" << listen_fd
              << std::endl;
    close(listen_fd);
  }
}

// --- Main Function ---
int main() {
  try {
    co_lib::Scheduler scheduler;

    std::cout << "Starting echo server coroutine..." << std::endl;
    // Add the main server loop as a coroutine
    co_lib::Coroutine *server_coro = scheduler.add_coroutine(
        // Capture scheduler by reference [&scheduler]
        [&scheduler](co_lib::Coroutine *self) {
          // Pass the address of the scheduler to server_loop
          server_loop(self, &scheduler, SERVER_PORT);
        }
        // Optional: specify stack size for server coroutine
        // , 256 * 1024 // e.g., 256KB stack
    );

    if (!server_coro) {
      std::cerr << "Fatal: Failed to create server coroutine." << std::endl;
      return 1;
    }

    std::cout << "Running scheduler..." << std::endl;
    scheduler.run(); // Blocks until all coroutines (server + handlers) finish

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
