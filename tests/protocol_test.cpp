#include "resp.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

int main() {
    const std::vector<std::string> args = {"ECHO", std::string("a\0b\r\n", 5)};
    const std::string frame = resp::encodeCommand(args);
    for (std::size_t n = 0; n < frame.size(); ++n) {
        assert(resp::parseCommand(std::string_view(frame).substr(0, n)).status ==
               resp::Status::Incomplete);
    }
    auto command = resp::parseCommand(frame + resp::encodeCommand({"PING"}));
    assert(command.status == resp::Status::Complete);
    assert(command.arguments == args);
    assert(command.consumed == frame.size());

    const std::vector<std::string> invalid = {
        "PING\r\n", "*0\r\n", "*129\r\n", "*-1\r\n", "*1\r\n+PING\r\n",
        "*1\r\n$65537\r\n", "*1\r\n$-1\r\n", "*1\r\n$3\r\nabcxx",
        "*1\n", "*1\rX", "*0000000000000000000001\r\n"
    };
    for (const auto& input : invalid) {
        assert(resp::parseCommand(input).status == resp::Status::Error);
    }

    const std::string largestBulk(resp::MaxBulk, 'x');
    assert(resp::parseCommand(resp::encodeCommand({"ECHO", largestBulk})).status ==
           resp::Status::Complete);
    const std::vector<std::string> maxArgs(resp::MaxArgs, "x");
    assert(resp::parseCommand(resp::encodeCommand(maxArgs)).arguments == maxArgs);
    // Each bulk is valid by itself, but the encoded command exceeds MaxFrame.
    const std::vector<std::string> oversized(16, largestBulk);
    assert(resp::parseCommand(resp::encodeCommand(oversized)).status == resp::Status::Error);

    const std::string reply = resp::bulk(args[1]);
    for (std::size_t n = 0; n < reply.size(); ++n) {
        assert(resp::parseReply(std::string_view(reply).substr(0, n)).status ==
               resp::Status::Incomplete);
    }
    const auto parsedReply = resp::parseReply(reply + resp::simple("PONG"));
    assert(parsedReply.status == resp::Status::Complete);
    assert(parsedReply.value == args[1]);
    assert(parsedReply.consumed == reply.size());
    assert(resp::parseReply(resp::simple("PONG")).value == "PONG");
    assert(resp::parseReply(resp::error("ERR test")).type == '-');
    assert(resp::parseReply(":1\r\n").integer == 1);
    assert(resp::parseReply(resp::nullBulk()).isNull);
    const auto complex = resp::array({resp::integer(-42), resp::nullBulk(),
                                     resp::array({resp::bulk("a\r\nb"), resp::simple("OK")})});
    for (std::size_t n = 0; n < complex.size(); ++n) {
        assert(resp::parseReply(std::string_view(complex).substr(0, n)).status == resp::Status::Incomplete);
    }
    const auto array = resp::parseReply(complex + resp::simple("NEXT"));
    assert(array.status == resp::Status::Complete && array.consumed == complex.size());
    assert(array.elements[0].integer == -42);
    assert(array.elements[1].isNull);
    assert(array.elements[2].elements[0].value == "a\r\nb");
    for (const auto& invalidReply : {":9223372036854775808\r\n", "$-2\r\n", "*-2\r\n",
                                   "*999999999\r\n", ":+1\r\n", ":\r\n", "?x\r\n"}) {
        assert(resp::parseReply(invalidReply).status == resp::Status::Error);
    }
    std::string nested;
    for (int i = 0; i < 34; ++i) nested += "*1\r\n";
    assert(resp::parseReply(nested + resp::nullBulk()).status == resp::Status::Error);
    std::cout << "Protocol tests passed\n";
}
