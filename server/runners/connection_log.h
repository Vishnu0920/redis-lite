#pragma once

#include <cstdio>

namespace connection_log {
// One stdio call per line keeps messages together across threaded clients.
inline void accepted(int fd) {
    std::fprintf(stderr, "Connection accepted (fd=%d)\n", fd);
}

inline void closing(int fd, const char* reason) {
    std::fprintf(stderr, "Connection closing (fd=%d): %s\n", fd, reason);
}

// Declare after the owning Socket so this logs before its destructor closes fd.
// Setup can transfer this responsibility to the connection's own CloseLog.
class CloseLog {
public:
    explicit CloseLog(int fd, const char* reason = "server stopped") : reason(reason), fd_(fd) {}
    ~CloseLog() { if (fd_ >= 0) closing(fd_, reason); }
    CloseLog(const CloseLog&) = delete;
    CloseLog& operator=(const CloseLog&) = delete;
    void release() { fd_ = -1; }

    const char* reason;
private:
    int fd_;
};
} // namespace connection_log
