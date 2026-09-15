#ifndef RAYLYRICS_IPC_CONTROL_H
#define RAYLYRICS_IPC_CONTROL_H

#include <string>

namespace raylyrics {

// Path of the Unix control socket ($XDG_RUNTIME_DIR/raylyrics.sock).
std::string ControlSocketPath();

// Non-blocking server used by the running overlay. Poll() returns the next
// command line, or an empty string when there is nothing to do.
class ControlServer {
public:
    ControlServer();
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    std::string Poll();

private:
    void Setup();
    void Teardown();

    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::string buffer_;
};

// Send one command to a running overlay. Returns false if it is not reachable.
bool SendControlCommand(const std::string& command);

}  // namespace raylyrics

#endif  // RAYLYRICS_IPC_CONTROL_H
