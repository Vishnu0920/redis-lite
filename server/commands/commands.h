#pragma once

#include "storage/storage.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// One processor per server, shared by all connections. Backend is replaceable.
class CommandProcessor {
public:
    explicit CommandProcessor(std::unique_ptr<storage::Storage> store);
    std::string execute(const std::vector<std::string>& args);
    std::optional<std::uint64_t> nextTTLDeadlineMS();
    void processTTLExpirations(std::uint64_t now);
private:
    std::unique_ptr<storage::Storage> store_;
    std::mutex mutex_; // Protect a complete command, never socket I/O.
};
