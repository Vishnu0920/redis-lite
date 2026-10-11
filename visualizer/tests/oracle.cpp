// Standalone reply oracle; not linked into the Redis Lite server.
#include "commands/commands.h"
#include "resp.h"
#include "storage/storage.h"

#include <iostream>
#include <iterator>
#include <memory>
#include <string>

int main() {
    CommandProcessor commands(storage::makeCustomStorage());
    commands.execute({"SET", "greeting", "hello"});
    commands.execute({"ZADD", "leaderboard", "10", "alice", "20", "bob"});
    std::string input(std::istreambuf_iterator<char>(std::cin), {});
    while (!input.empty()) {
        const auto parsed = resp::parseCommand(input);
        if (parsed.status == resp::Status::Incomplete) return 2;
        const std::string reply = parsed.status == resp::Status::Error
            ? resp::error("ERR Protocol error: " + parsed.error)
            : commands.execute(parsed.arguments);
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char byte : reply) std::cout << hex[byte >> 4] << hex[byte & 15];
        std::cout << '\n';
        if (parsed.status == resp::Status::Error) break;
        input.erase(0, parsed.consumed);
    }
}
