#include "runners/runners.h"
#include "runners/connection_log.h"
#include "commands/commands.h"
#include "buffers/input_buffer.h"
#include "timers/idle_list.h"
#include "timers/time.h"
#include "net.h"
#include "resp.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <utility>
#include <vector>

namespace {
enum class State { Request, Response, End };

template <typename Buffer>
struct Conn {
    Conn(net::Socket socket, CommandProcessor& commands)
        : socket(std::move(socket)), commands(commands), closeLog(this->socket.get()) {}

    net::Socket socket;
    CommandProcessor& commands;
    State state = State::Request; //basically like want_read, wat_write, want_close
    Buffer input;   // Incomplete request and any following pipelined bytes.
    std::string output;  // One reply at a time: slow readers cannot grow a reply queue.
    std::size_t sent = 0;
    bool closeAfterWrite = false;
    structures::ListNode<Conn<Buffer>> idle_node;
    std::uint64_t last_active_ms = 0;
    connection_log::CloseLog closeLog;
};

// Like the sample's fd2Conn: index by fd, with empty slots for unused descriptors.
template <typename Buffer>
using Connections = std::vector<std::unique_ptr<Conn<Buffer>>>;
constexpr std::size_t IoBudget = 64 * 1024; // Bytes per read/write handler turn.
constexpr unsigned AcceptBudget = 64;      //Per turn at max we will accpet 64 connections/call handleConnections() 64 times(we dont want to spend all the time just accepting new connections)

// Runner-lifetime state: connections own sockets; the idle list borrows them.
// The shared processor owns the database and its TTL heap.
template <typename Buffer>
struct EventLoopState {
    CommandProcessor& commands;
    std::uint64_t idleTimeoutMS;
    Connections<Buffer> fd2Conn;
    structures::IntrusiveList<Conn<Buffer>, &Conn<Buffer>::idle_node> idleList;
};

template <typename Buffer>
void refreshTimer(EventLoopState<Buffer>& state, Conn<Buffer>& conn) {
    conn.last_active_ms = timers::monotonicMS();
    state.idleList.moveToBack(conn);
}

template <typename Buffer>
void destroyConnection(EventLoopState<Buffer>& state, Conn<Buffer>& conn) {
    const int fd = conn.socket.get();
    state.idleList.erase(conn); // Unlink before the Conn (and its node) dies.
    state.fd2Conn[fd].reset(); // Socket's destructor closes the descriptor.
}

template <typename Buffer>
int nextTimerMS(EventLoopState<Buffer>& state) {
    auto deadline = state.commands.nextTTLDeadlineMS(); // Heap root: O(1).
    if (const auto* conn = state.idleList.front()) {
        const auto idleDeadline = conn->last_active_ms + state.idleTimeoutMS;
        if (!deadline || idleDeadline < *deadline) deadline = idleDeadline;
    }
    return timers::pollTimeoutMS(deadline, timers::monotonicMS());
}

template <typename Buffer>
void processIdleTimeouts(EventLoopState<Buffer>& state, std::uint64_t now) {
    while (auto* conn = state.idleList.front()) {
        if (conn->last_active_ms + state.idleTimeoutMS > now) break;
        conn->closeLog.reason = "idle timeout";
        destroyConnection(state, *conn);
    }
}

template <typename Buffer>
void processTimers(EventLoopState<Buffer>& state) {
    const auto now = timers::monotonicMS();
    processIdleTimeouts(state, now);
    // Bound TTL deletions per turn. If more are due, nextTimerMS returns zero
    // and poll checks I/O readiness before the next expiration batch.
    state.commands.processTTLExpirations(now);
}

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool wouldBlock() { //checks whether the failure is due to no data in the read/write buffer
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

/*
Return true if parsing changed the state. This never touches the socket.
This basically does: Try to parse one command from the connections's input buffer. If command is available execute it prepare the repsonse in output buffer.

*/
template <typename Buffer>
bool prepareResponse(Conn<Buffer>& conn) {
    auto result = resp::parseCommand(conn.input.view());
    if (result.status == resp::Status::Incomplete) return false;

    if (result.status == resp::Status::Error) {
        conn.output = resp::error("ERR Protocol error: " + result.error);
        conn.input.clear();
        conn.closeAfterWrite = true;
    }

    else {
        conn.output = conn.commands.execute(result.arguments);
        conn.input.consume(result.consumed);
    }
    conn.sent = 0;
    conn.state = State::Response;
    return true;
}

template <typename Buffer>
void handleRead(Conn<Buffer>& conn, bool& activity) {
    std::size_t received = 0;
    while (conn.state == State::Request) {
        // Parse before recv, including after the last read of this turn. Thus a
        // complete buffered request never waits for another POLLIN notification.
        if (prepareResponse(conn)) return; //if the command is parsed and executed return immediately(since our designis to send reply before processing another request)
        if (received >= IoBudget) return;

        char buffer[4096];
        const std::size_t count = std::min(sizeof(buffer), IoBudget - received);
        const ssize_t n = ::recv(conn.socket.get(), buffer, count, 0);
        if (n > 0) {
            activity = true;
            conn.input.append(buffer, static_cast<std::size_t>(n));
            received += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && wouldBlock()) return; // this case is when the kernel input buffer is emtpy-->Retain input and wait for POLLIN.
        if (n < 0) std::perror("recv");
        conn.closeLog.reason = n < 0 ? "receive error" : "client disconnected";
        if (n == 0 && !conn.input.empty()) {
            conn.closeLog.reason = "client disconnected during incomplete request";
            std::cerr << "Client closed during a command\n";
        }
        conn.state = State::End;
    }
}

template <typename Buffer>
void handleWrite(Conn<Buffer>& conn, bool& activity) {
    std::size_t written = 0;
    while (conn.sent < conn.output.size() && written < IoBudget) {
        const std::size_t count = std::min(conn.output.size() - conn.sent, IoBudget - written);
        const ssize_t n = ::send(conn.socket.get(), conn.output.data() + conn.sent, count, MSG_NOSIGNAL);

        if (n > 0) {
            activity = true;
            conn.sent += static_cast<std::size_t>(n);
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;

        /*
        This is the case when we do an optimistic write call and the wirte buffer is full
        Ideally we would never call write when the buffer is full(because we check the POLLOUT flag for the connection)
        But in the optimistic wirte case we just call the wirte without checking and if the buffer is full then we just immediately return
        and in the next poll() call the POLLOUT flag for this will be set and it will be handled accordingly
        */
        if (n < 0 && wouldBlock()) return; // Retain the unsent suffix for POLLOUT.



        if (n < 0) std::perror("send");
        conn.closeLog.reason = n < 0 ? "send error" : "send returned zero";
        conn.state = State::End;
        return;
    }
    if (conn.sent != conn.output.size()) return;

    conn.output.clear();
    conn.sent = 0;
    if (conn.closeAfterWrite) conn.closeLog.reason = "protocol error";
    conn.state = conn.closeAfterWrite ? State::End : State::Request;
    if (conn.state == State::Request) {
        // A pipeline may already contain the next complete command. Queue its
        // reply now so poll watches POLLOUT even if no new bytes arrive.
        prepareResponse(conn);
    }
}

template <typename Buffer>
bool handleNewConnections(int listener, EventLoopState<Buffer>& state) {
    auto& fd2Conn = state.fd2Conn;
    for (unsigned accepted = 0; accepted < AcceptBudget;) {
        net::Socket socket(::accept(listener, nullptr, nullptr));
        const int fd = socket.get();
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (wouldBlock()) return true;
            if (errno == ECONNABORTED) {
                ++accepted; // A canceled connection still counts against this turn.
                continue;
            }
            std::perror("accept");
            // In particular, do not spin on a readable listener after fd
            // exhaustion. Match threaded mode's exit on a fatal accept failure.
            return false;
        }
        ++accepted;
        connection_log::accepted(fd);
        connection_log::CloseLog setupLog(fd, "connection setup failed");
        if (!setNonBlocking(fd)) {
            std::perror("fcntl client");
            continue; // Socket closes automatically.
        }
        if (fd2Conn.size() <= static_cast<std::size_t>(fd)) {
            fd2Conn.resize(static_cast<std::size_t>(fd) + 1);
        }
        fd2Conn[fd] = std::make_unique<Conn<Buffer>>(std::move(socket), state.commands);
        setupLog.release(); // Conn now logs its own closure.
        fd2Conn[fd]->last_active_ms = timers::monotonicMS();
        state.idleList.pushBack(*fd2Conn[fd]);
    }
    return true;
}
template <typename Buffer>
int runEventLoop(int listener, CommandProcessor& commands, std::uint64_t idleTimeoutMS) {
    if (!setNonBlocking(listener)) {
        std::perror("fcntl listener");
        return 1;
    }

    EventLoopState<Buffer> state{commands, idleTimeoutMS, {}, {}};
    auto& fd2Conn = state.fd2Conn;
    std::vector<pollfd> pollfds;
    while (true) {
        pollfds.clear();
        pollfds.push_back({listener, POLLIN, 0});
        for (const auto& conn : fd2Conn) {
            if (!conn) continue;
            // Request wants read; Response wants write. Do not watch POLLOUT on
            // idle clients: writable sockets would otherwise keep waking poll.
            const short events = conn->state == State::Request ? POLLIN : POLLOUT;
            pollfds.push_back({conn->socket.get(), events, 0});
        }

        if (::poll(pollfds.data(), pollfds.size(), nextTimerMS(state)) < 0) {
            if (errno == EINTR) {
                processTimers(state);
                continue;
            }
            std::perror("poll");
            return 1;
        }

        if (pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Listening socket failed\n";
            return 1;
        }
        if ((pollfds[0].revents & POLLIN) &&
            !handleNewConnections(listener, state)) return 1;

        // The snapshot contains only clients present before poll; new clients
        // enter the next snapshot. Heap-owned Conn objects survive vector growth.
        for (std::size_t i = 1; i < pollfds.size(); ++i) {
            const auto& pfd = pollfds[i];
            if (!pfd.revents) continue;
            auto& conn = *fd2Conn[pfd.fd];
            bool activity = false;
            try {
                if (pfd.revents & (POLLERR | POLLNVAL)) {
                    conn.closeLog.reason = pfd.revents & POLLNVAL ? "invalid descriptor" : "socket error";
                    conn.state = State::End;
                }

                else {
                    // HUP can accompany unread data; recv must drain it before
                    // observing EOF. A write-side half-close still gets replies.
                    if (conn.state == State::Request &&
                        (pfd.revents & (POLLIN | POLLHUP))) handleRead(conn, activity);
                    if (conn.state == State::Response &&
                        (pfd.revents & (POLLIN | POLLOUT | POLLHUP))) handleWrite(conn, activity); //if any 3 of these events are truen it will execute

                    /*
                    We have implemented optimistic non blocking write here. First we check whether a connection is in request state then we check
                    whether the revents has the POLLIN flag if yes we read the data.
                    Then again immediately we are checking if the the connection is in Response state and has the POLLOUT flag(or any of the other 2 as well) and if yes we write immediately
                    Here we can see that we are not waiting for the next poll() call. We immediately try to write the message
                    */
                }
            }
            catch (const std::exception& e) {
                std::fprintf(stderr, "Client handler failed (fd=%d): %s\n", pfd.fd, e.what());
                conn.closeLog.reason = "handler exception";
                conn.state = State::End;
            }
            if (conn.state == State::End) destroyConnection(state, conn);
            else if (activity) refreshTimer(state, conn);
        }
        processTimers(state);
    }
}
} // namespace

int runEventLoopServer(int listener, std::shared_ptr<CommandProcessor> commands,
                       InputBufferMode bufferMode, std::uint64_t idleTimeoutMS) {
    // Select once at startup; buffer operations have no runtime mode branch.
    if (bufferMode == InputBufferMode::Regular) {
        return runEventLoop<RegularInputBuffer>(listener, *commands, idleTimeoutMS);
    }
    return runEventLoop<InputBuffer>(listener, *commands, idleTimeoutMS);
}
