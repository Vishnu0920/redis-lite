#include "runners/runners.h"
#include "runners/connection_log.h"
#include "commands/commands.h"
#include "net.h"
#include "resp.h"

#include <cstdio>
#include <algorithm>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <thread>

namespace {
void handleClient(int fd, std::shared_ptr<CommandProcessor> commands) {
    net::Socket connection(fd); //creates a new Socket object called connection using this socket fd-->this connection object now owns the fd and fd's life cycle is tied to scope of connection
    connection_log::CloseLog closeLog(fd, "handler exception");

    try {//put the client handlin code in a try block-->potentially memory alocation issues etc are possible

        /*
        We will initalise the persistent recieve buffer for this clinet-->input.
        We will store all the incoming bytes in this string only
        hence it is initalised outside the while loop
        */
        std::string input;
        while (true) {

            /*
            Here we can see that we first parse before even recieving--this i sjust in case ot clear the bufferif anyhting already exists
            */
            auto result = resp::parseCommand(input);

            //if after parsing there is an error then we send the appropriate error response and return the function
            if (result.status == resp::Status::Error) {
                closeLog.reason = "protocol error";
                if (!net::sendAll(fd, resp::error("ERR Protocol error: " + result.error))) {
                    closeLog.reason = "send error while reporting protocol error";
                }
                return; // Socket destructor closes the connection.
            }

            //if after parsing the command is parsed completed then the status is complete-->hence we will execute the command
            if (result.status == resp::Status::Complete) {
                if (!net::sendAll(fd, commands->execute(result.arguments))) {
                    closeLog.reason = "send error";
                    return;
                }
                input.erase(0, result.consumed); //remove the parsed and executed command from the inout string buffer
                continue; // Process another buffered command before receiving.
            }

            //If the parser status returns incomplete we reach here
            const ssize_t n = net::receiveMore(fd, input); //recieve bytes from the client, in the next iteration if full command recieve it will parse else again we will recieve more bytes
            if (n < 0) { //if any error in recieving the bytes we log the error and return immediately
                closeLog.reason = "receive error";
                std::perror("recv");
                return;
            }

            if (n == 0) { //if n=0 it means the client closed-hence we log that and return
                closeLog.reason = input.empty() ? "client disconnected" : "client disconnected during incomplete request";
                if (!input.empty()) std::cerr << "Client closed during a command\n";
                return;
            }
        }
    }
    catch (const std::exception& e) {//catches any excpetion and prints the exception
        closeLog.reason = "handler exception";
        std::fprintf(stderr, "Client handler failed (fd=%d): %s\n", fd, e.what());
    }
}

} // namespace

int runThreadedServer(int listener, std::shared_ptr<CommandProcessor> commands) {
    // A connection can disappear between poll and accept. Keep that accept
    // nonblocking so it cannot prevent TTL maintenance. On Linux, accepted
    // sockets do not inherit O_NONBLOCK: worker I/O remains blocking.
    const int flags = ::fcntl(listener, F_GETFL, 0);
    if (flags < 0 || ::fcntl(listener, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::perror("fcntl listener");
        return 1;
    }
    while (true) {
        // Workers can install an earlier TTL while this thread waits. Cap its
        // sleep at 100 ms so active expiration also works without new clients.
        // Worker socket I/O remains blocking, with no idle timeout in this mode.
        const int next = timers::pollTimeoutMS(commands->nextTTLDeadlineMS(), timers::monotonicMS());
        pollfd pending{listener, POLLIN, 0};
        if (::poll(&pending, 1, next < 0 ? 100 : std::min(next, 100)) < 0) {
            if (errno == EINTR) {
                commands->processTTLExpirations(timers::monotonicMS());
                continue;
            }
            std::perror("poll listener");
            return 1;
        }
        commands->processTTLExpirations(timers::monotonicMS());
        if (pending.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Listening socket failed\n";
            return 1;
        }
        if (!(pending.revents & POLLIN)) continue;
        const int fd = ::accept(listener, nullptr, nullptr); //when a client connection is available accept the connection and save the socket's fd
        if (fd < 0) {//check if there is an error in the connection socket
            if (errno == EINTR) continue; //if the error is because of an SIGNAL interrupting the accept syscall then we can retry -->hence we just continue(start a new iteration of loop and try to accespt the connection again)
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) continue;

            //if any other error then log the error and terminate the main()
            std::perror("accept");
            return 1;
        }

        connection_log::accepted(fd);
        //create a new worker thread for each of the new connections and pass it to handleClient
        try {
            std::thread worker(handleClient, fd, commands);
            worker.detach(); //detach the worker thread so we dont need to manually join the thread
        }

        //if error in creating the wokrer thread then close the connection to the
        catch (const std::exception& e) {
            connection_log::closing(fd, "worker creation failed");
            ::close(fd);
            std::fprintf(stderr, "Cannot start client thread (fd=%d): %s\n", fd, e.what());
        }
    }
}
