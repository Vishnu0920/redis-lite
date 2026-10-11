#include "buffers/input_buffer.h"
#include "resp.h"

#include <cassert>
#include <iostream>
#include <string>
#include <type_traits>

template <typename Buffer>
void checkBuffer() {
    Buffer buffer;
    assert(buffer.empty());

    // Both strategies retain the next frame; offset consumption avoids copies.
    const auto ping = resp::encodeCommand({"PING"});
    const auto echo = resp::encodeCommand({"ECHO", std::string("a\0b", 3)});
    const auto pipeline = ping + echo;
    buffer.append(pipeline.data(), pipeline.size());
    const char* original = buffer.view().data();
    auto parsed = resp::parseCommand(buffer.view());
    buffer.consume(parsed.consumed);
    if constexpr (std::is_same_v<Buffer, InputBuffer>) {
        assert(buffer.view().data() == original + ping.size());
    }
    assert(buffer.view() == echo);
    parsed = resp::parseCommand(buffer.view());
    assert(parsed.arguments[1] == std::string("a\0b", 3));
    buffer.consume(parsed.consumed);
    assert(buffer.empty());

    // Repeated reuse, compaction, and growth must preserve FIFO byte order.
    std::string expected(8192, 'x');
    buffer.append(expected.data(), expected.size());
    for (std::size_t i = 0; i < 100; ++i) {
        const std::size_t consumed = i % 2 == 0 ? expected.size() - 1 : 1;
        buffer.consume(consumed);
        expected.erase(0, consumed);
        const std::string added(4096 + i, static_cast<char>(i));
        buffer.append(added.data(), added.size());
        expected += added;
        assert(buffer.view() == expected);
    }
    buffer.clear();
    assert(buffer.empty());

    // A partial next command survives consuming a preceding complete command.
    const auto partial = ping + echo.substr(0, echo.size() - 2);
    buffer.append(partial.data(), partial.size());
    buffer.consume(resp::parseCommand(buffer.view()).consumed);
    assert(resp::parseCommand(buffer.view()).status == resp::Status::Incomplete);
    buffer.append("\r\n", 2);
    assert(resp::parseCommand(buffer.view()).arguments[1] == std::string("a\0b", 3));
}

int main() {
    checkBuffer<RegularInputBuffer>();
    checkBuffer<InputBuffer>();
    std::cout << "Input buffer tests passed (regular and offset)\n";
}
