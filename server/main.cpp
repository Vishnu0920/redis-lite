#include "net.h"
#include "runners/runners.h"
#include "commands/commands.h"

#include <arpa/inet.h>
#include <charconv>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <string_view>

int main(int argc, char* argv[]) {
    bool eventLoop = false; // Keep the Chapter 4 behavior as the default.
    InputBufferMode bufferMode = InputBufferMode::Offset;
    bool modeSpecified = false;
    bool bufferSpecified = false;
    bool idleTimeoutSpecified = false;
    std::uint64_t idleTimeoutMS = timers::DefaultIdleTimeoutMS;
    auto usage = [&](std::ostream& output) {
        output << "Usage: " << argv[0]
               << " [--mode threaded|event-loop] [--buffer regular|offset] [--idle-timeout-ms N]\n"
               << "--buffer requires --mode event-loop (default buffer: offset).\n"
               << "--idle-timeout-ms requires --mode event-loop (default: 60000; positive integer <= INT_MAX).\n";
    };
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) {
            usage(std::cerr);
            return 1;
        }
        const std::string_view option(argv[i]);
        const std::string_view value(argv[i + 1]);
        if (option == "--mode" && !modeSpecified &&
            (value == "threaded" || value == "event-loop")) {
            eventLoop = value == "event-loop";
            modeSpecified = true;
        } else if (option == "--buffer" && !bufferSpecified &&
                   (value == "regular" || value == "offset")) {
            bufferMode = value == "regular" ? InputBufferMode::Regular : InputBufferMode::Offset;
            bufferSpecified = true;
        } else if (option == "--idle-timeout-ms" && !idleTimeoutSpecified) {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), idleTimeoutMS);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                idleTimeoutMS == 0 || idleTimeoutMS > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                usage(std::cerr);
                return 1;
            }
            idleTimeoutSpecified = true;
        } else {
            usage(std::cerr);
            return 1;
        }
    }
    if (bufferSpecified && !eventLoop) {
        std::cerr << "--buffer requires --mode event-loop.\n";
        usage(std::cerr);
        return 1;
    }
    if (idleTimeoutSpecified && !eventLoop) {
        std::cerr << "--idle-timeout-ms requires --mode event-loop.\n";
        usage(std::cerr);
        return 1;
    }

    net::Socket listener(::socket(AF_INET, SOCK_STREAM, 0)); //sets up the listening socket
    if (listener.get() < 0) { 
        std::perror("socket"); 
        return 1; 
    }
    
    /*
    Here we are configuring the reuse of the same port for the socket
    For example if we stop the server and immediately restart it the during binding we might get an error sayin Address already in use
    This allows us to reuse the same address for this socket
    why &reuse is passed? --> for the parameter the setsockopt takes a pointer to the memory containing the option value
    So we are effectively saying-->Here is the address of a 4-byte integer containing 1. Use it to enable SO_REUSEADDR on this socket.
    */
    int reuse = 1;
    if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::perror("setsockopt"); 
        return 1;
    }

    //configure the address
    sockaddr_in address{}; //initalise the sockaddr_in struct
    address.sin_family = AF_INET; //for IPv4 address
    address.sin_port = htons(net::Port); //sets the correct port no(6739) after converting to big endian
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); //here we are setting the IP address it should bind to-->INADDR_LOOPBACK refers to 127.0.0.1 which means current deivce Ip address
    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address),sizeof(address)) < 0) { //bind the socket with the address
        std::perror("bind"); 
        return 1;
    }

    //start the listening-->upto 16 that can wait in queueu to be accepted(doesnt mean ony 16 connetions at a time)
    if (::listen(listener.get(), 16) < 0) { 
        std::perror("listen"); 
        return 1; 
    }


    std::cout << "Listening on 127.0.0.1:" << net::Port
              << " (" << (eventLoop ? "event-loop" : "threaded");
    if (eventLoop) {
        std::cout << ", buffer=" << (bufferMode == InputBufferMode::Regular ? "regular" : "offset");
        std::cout << ", idle-timeout-ms=" << idleTimeoutMS;
    }
    std::cout << ")" << std::endl;
    try {
        auto commands = std::make_shared<CommandProcessor>(storage::makeCustomStorage());
        return eventLoop ? runEventLoopServer(listener.get(), commands, bufferMode, idleTimeoutMS)
                         : runThreadedServer(listener.get(), commands);
    } catch (const std::exception& e) {
        std::cerr << "Server failed: " << e.what() << '\n';
        return 1;
    }
}
