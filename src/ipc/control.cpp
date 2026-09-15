#include "ipc/control.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

namespace raylyrics {

std::string ControlSocketPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') {
        return std::string(runtime) + "/raylyrics.sock";
    }
    return "/tmp/raylyrics-" + std::to_string(static_cast<long long>(getuid())) + ".sock";
}

ControlServer::ControlServer() { Setup(); }

ControlServer::~ControlServer() { Teardown(); }

void ControlServer::Setup() {
    const std::string path = ControlSocketPath();
    unlink(path.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) return;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    listen(listen_fd_, 4);
}

void ControlServer::Teardown() {
    if (client_fd_ >= 0) close(client_fd_);
    if (listen_fd_ >= 0) close(listen_fd_);
    unlink(ControlSocketPath().c_str());
    client_fd_ = -1;
    listen_fd_ = -1;
}

std::string ControlServer::Poll() {
    if (listen_fd_ < 0) return {};

    if (client_fd_ < 0) {
        const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) client_fd_ = fd;
    }
    if (client_fd_ < 0) return {};

    char buffer[512];
    const ssize_t count = read(client_fd_, buffer, sizeof(buffer));
    if (count > 0) {
        buffer_.append(buffer, static_cast<std::size_t>(count));
    } else if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(client_fd_);
        client_fd_ = -1;
        buffer_.clear();
        return {};
    }

    const std::size_t newline = buffer_.find('\n');
    if (newline == std::string::npos) return {};

    std::string line = buffer_.substr(0, newline);
    buffer_.erase(0, newline + 1);
    close(client_fd_);
    client_fd_ = -1;
    return line;
}

bool SendControlCommand(const std::string& command) {
    const std::string path = ControlSocketPath();
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return false;
    }

    const std::string payload = command + "\n";
    const ssize_t written = write(fd, payload.data(), payload.size());
    close(fd);
    return written == static_cast<ssize_t>(payload.size());
}

}  // namespace raylyrics
