#pragma once

#include <cerrno>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace net {
constexpr unsigned short Port = 6379;

/*
We will create a separate socket class so that even if there is an exception somehwere and the code returns before closing of the socket
it will be automatically closed becasue the socket will be in the scope of this object hence the socket will be acquired on creation and 
released automatically when this object's scope is exited-RAII
*/
class Socket {
    int fd_; //The socket object will contain the file descriptor to the socket. This is private by default because this is a class
public:
    explicit Socket(int fd) : fd_(fd) {} //here we are doing member initialisation
    ~Socket() { if (fd_ >= 0) ::close(fd_); } //here we re asaying if the fd is valid(ie the socket is open) then we close it. ::close() means we are invoking the close() function fomr the global namespace not belomging to socket class
    
    /*
    We will prevent the copy constructor and copy assigment operators fro working
    This is because if we allow that then we can have 2 Socket objects that might owm the same socket fd and can cause double deletion/
    if the fd was assigned to a different scoket then one of them may delete it causing issues
    */
    Socket(const Socket&) = delete; //prevents the copy constructor- Socket s2=s1; -->will give error
    Socket& operator=(const Socket&) = delete; //prevents assignement Socket s1(5), Socket s2(6); s1=s2-->will give error

    // Transfer ownership to an event-loop connection without duplicating the fd.
    Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    
    int get() const { return fd_; } //returns the fd
};

// sendAll is for blocking sockets only. The event loop has its own write handler.

/*
This function basically guarantees that the entire message is sent. send() doesnt guarantee this
the inline keyword-because this fucntion defintion is present in a .h(header) file that many other .cpp files may use, and these .cpp files
may have their own snedAll() functions ie function with same name, hence to prevent this form clashing with that we use sendAll here
*/
inline bool sendAll(int fd, std::string_view data) {
    std::size_t offset = 0; //This is the variable that tracks how many bytes of data have already been sent
    
    while (offset < data.size()) { //the loop coninues till all the bytes have been sent
        //The MSG_NOSIGNAL argument ensres that even if the listineing socket/connection is closed that doesnt cause the current process to terminate
        const ssize_t n = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL); //send bytes starting from the first unsent byte(data.data()+offset) for totalsize-offset no of bytes
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

// > 0: bytes appended, 0: peer closed, -1: socket error.
/*
recieveMore recieves bytes from atmost one recv() only.
It doesnt keep calling repeat till the entire command is recieved(it doesnt know when a full command has arrived or not-that is the parseing job)
Here if an error occurs due to system call being interrupted in that case we keep retrying till we get some data or some other error
*/
inline ssize_t receiveMore(int fd, std::string& input) {
    char buffer[4096];
    ssize_t n;
    do {
        n = ::recv(fd, buffer, sizeof(buffer), 0); //This is a blocking call-hence if the input buffer has no data then the thread is put in blocked state
    } 
    /*
    Now when the thread was in blocked state/just before it could perform the system call(recv) if any signal arrives and interrupts this system call causing
    it to not be completed then this is called system call interrupt and this sets the errno=EINTR(which says the system call was interrupted by a signal)
    So now since this recv() wasnt completed we again retry it till we finally get some data(no of bytes(n)>0)
    Baiscally if errno=EINTR it means that the recv() didnt fail because of any network.socket issue, it means that wehn this recv() was
    being executed a signal interrupted it hence casuing it to stop-hence we can retry it.
    One more thing errno-->thread local ie every thread has its own erro from which we can identify what and why made a thread stop
    */
    while (n < 0 && errno == EINTR); 
    if (n > 0) input.append(buffer, static_cast<std::size_t>(n));//the input variable is persistent storage across multiple recieve operations
    return n;
}
}
