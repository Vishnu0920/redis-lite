#include "commands/commands.h"
#include "resp.h"

#include <cassert>
#include <iostream>
#include <thread>

int main() {
    CommandProcessor commands(storage::makeCustomStorage());
    assert(commands.execute({"GET", "missing"}) == resp::nullBulk());
    assert(commands.execute({"set", "name", "value"}) == "+OK\r\n");
    assert(commands.execute({"GET", "name"}) == resp::bulk("value"));
    assert(commands.execute({"DEL", "name", "name", "missing"}) == ":1\r\n");
    assert(commands.execute({"KEYS", "*"}) == "*0\r\n");
    assert(commands.execute({"ZADD", "board", "2", "bob", "1", "alice"}) == ":2\r\n");
    assert(commands.execute({"ZRANK", "board", "bob"}) == ":1\r\n");
    assert(commands.execute({"ZSCORE", "board", "missing"}) == resp::nullBulk());
    assert(commands.execute({"ZQUERY", "board", "-inf", "", "0", "2"}) ==
           resp::array({resp::bulk("alice"), resp::bulk("1"), resp::bulk("bob"), resp::bulk("2")}));
    // Validate every score before applying any changes from a multi-pair ZADD.
    for (const char* invalid : {"", "nan", "abc", "1e999", " 1", "1 "}) {
        auto result = commands.execute({"ZADD", "board", "9", "bob", invalid, "new"});
        assert(result.front() == '-');
        assert(commands.execute({"ZSCORE", "board", "bob"}) == resp::bulk("2"));
        assert(commands.execute({"ZSCORE", "board", "new"}) == resp::nullBulk());
    }
    assert(commands.execute({"ZADD", "board", std::string("1\0x", 3), "new"}).front() == '-');
    assert(commands.execute({"ZQUERY", "board", "0", "", "9223372036854775808", "1"}).front() == '-');
    assert(commands.execute({"ZQUERY", "board", "0", "", "-9223372036854775808", "1"}) == "*0\r\n");
    assert(commands.execute({"ZQUERY", "board", "0", "", "0", "-1"}) == "*0\r\n");
    assert(commands.execute({"GET", "board"}).find("WRONGTYPE") != std::string::npos);
    assert(commands.execute({"SET", "board", "string"}) == "+OK\r\n");
    assert(commands.execute({"ZREM", "board", "bob"}).find("WRONGTYPE") != std::string::npos);
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"GET"}, {"SET", "key"}, {"DEL"}, {"ZADD", "key", "1"},
             {"ZREM", "key"}, {"ZSCORE", "key"}, {"ZRANK"}, {"ZCARD"}, {"ZQUERY"}}) {
        assert(commands.execute(args).find("wrong number") != std::string::npos);
    }
    // Concurrent clients share one synchronized database and sorted set.
    std::vector<std::thread> threads;
    for (int client = 0; client < 8; ++client) {
        threads.emplace_back([&, client] {
            for (int i = 0; i < 500; ++i) {
                const auto member = std::to_string(client) + ":" + std::to_string(i);
                assert(commands.execute({"ZADD", "shared", std::to_string(i), member}) == ":1\r\n");
            }
        });
    }
    for (auto& thread : threads) thread.join();
    assert(commands.execute({"ZCARD", "shared"}) == ":4000\r\n");
    // Oversized arrays produce a command error, keeping the connection usable.
    for (int i = 0; i < 17; ++i) {
        commands.execute({"SET", std::string(resp::MaxBulk - 2, 'x') + std::to_string(i), "v"});
    }
    assert(commands.execute({"KEYS"}) == "-ERR response exceeds 1 MiB\r\n");
    assert(commands.execute({"PING"}) == "+PONG\r\n");
    std::cout << "Command tests passed (validation, types, shared storage, reply limits)\n";
}
