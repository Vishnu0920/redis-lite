#include "net.h"
#include "resp.h"

#include <arpa/inet.h>
#include <charconv>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

void printReply(const resp::ReplyResult& reply, const std::string& indent = "") {
    if (reply.isNull) std::cout << "(nil)\n";
    else if (reply.type == ':') std::cout << "(integer) " << reply.integer << '\n';
    else if (reply.type == '*') {
        if (reply.elements.empty()) std::cout << "(empty array)\n";
        for (std::size_t i = 0; i < reply.elements.size(); ++i) {
            std::cout << indent << i + 1 << ") ";
            printReply(reply.elements[i], indent + "   ");
        }
    } else {
        if (reply.type == '-') std::cout << "(error) ";
        std::cout << reply.value << '\n';
    }
}

// Interpret timer replies using the command that produced them. Other commands
// (and all RESP errors) keep the usual generic reply display.
bool printTimerReply(const std::vector<std::string>& args, const resp::ReplyResult& reply) {
    if (args.empty() || reply.type != ':') return false;
    std::string name = args[0];
    for (char& ch : name) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');

    if (name == "TTL" && args.size() == 2) {
        if (reply.integer == -1) std::cout << "No timer for this key.\n";
        else if (reply.integer == -2) std::cout << "Timer expired or key does not exist.\n";
        else if (reply.integer == 0) std::cout << "Less than 1 second remaining.\n";
        else if (reply.integer > 0) {
            std::cout << "Time remaining: " << reply.integer
                      << (reply.integer == 1 ? " second.\n" : " seconds.\n");
        } else return false;
        return true;
    }
    if (name == "EXPIRE" && args.size() == 3 && (reply.integer == 0 || reply.integer == 1)) {
        std::int64_t seconds;
        const auto& text = args[2];
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
        if (reply.integer == 0) {
            std::cout << "Key does not exist or has expired; no timer was set.\n";
        } else if (seconds <= 0) {
            std::cout << "Key deleted immediately (nonpositive TTL).\n";
        } else {
            std::cout << "Timer set to " << seconds << (seconds == 1 ? " second.\n" : " seconds.\n");
        }
        return true;
    }
    if (name == "PERSIST" && args.size() == 2 && (reply.integer == 0 || reply.integer == 1)) {
        if (reply.integer == 1) std::cout << "Timer removed; this key has no expiration.\n";
        else std::cout << "No timer to remove (key has no timer or does not exist).\n";
        return true;
    }
    return false;
}

bool query(int fd, const std::vector<std::string>& args, std::string& input) {
    if (args.empty() || args.size() > resp::MaxArgs) {
        std::cerr << "Command must have 1..128 arguments\n"; 
        return true;
    }

    for (const auto& arg : args) {
        if (arg.size() > resp::MaxBulk) {
            std::cerr << "Argument exceeds 64 KiB\n"; 
            return true;
        }
    }

    const std::string request = resp::encodeCommand(args);
    if (request.size() > resp::MaxFrame) {
        std::cerr << "Command exceeds 1 MiB\n"; 
        return true;
    }

    if (!net::sendAll(fd, request)) { 
        std::perror("send"); 
        return false; 
    }

    while (true) {
        auto reply = resp::parseReply(input); //first parse the input to check if anything is remaining and consume that
        if (reply.status == resp::Status::Error) { //if error then log the error and return
            std::cerr << "Invalid server reply: " << reply.error << '\n';
            return false;
        }

        if (reply.status == resp::Status::Complete) { //if the repsonse is complete that means we have recieved the full response
            if (!printTimerReply(args, reply)) printReply(reply);
            input.erase(0, reply.consumed); //clear the input buffer uptill whatever is consumed
            return true;
        }

        //if the status is incomplete then we need to keep reciebing more bytes
        const ssize_t n = net::receiveMore(fd, input);
        if (n < 0) { std::perror("recv"); return false; }
        if (n == 0) {
            std::cerr << "Server closed before a complete reply\n"; return false;
        }
    }
}


/*
argc-->count of command line arguments adn argv-->is the vector that holds these arguments
Eg- if we do ./client-->argc=1 and argv[0]=./client
    but if we do ./client ECHO "Hello World", argc=3, argv={./client, ECHO, Hello World}
*/
int main(int argc, char* argv[]) {
   
    net::Socket connection(::socket(AF_INET, SOCK_STREAM, 0)); //name of the socket object is connection
    if (connection.get() < 0) { 
        std::perror("socket"); 
        return 1; 
    }
    
    //here again we we are declaring struct that holds the address of server(this is not address of the client)
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(net::Port);
    
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) return 1; //convert IP address from string to binary representation and store in address.sin_addr
    
    if (::connect(connection.get(), reinterpret_cast<sockaddr*>(&address),sizeof(address)) < 0) { //Connect with the server using the address
        std::perror("connect"); 
        return 1;
    }
    
    std::string input;
    /*
    if the command is passed directly as the command line argument itself- ./client ECHO "Hello World" then its jsut on single command we dont enter into the while loop 
    */
    if (argc > 1) {
        std::vector<std::string> args(argv + 1, argv + argc); //Extract the actual command arguments(the command name and its arguments and leave out the file name ./client and stor in args)
        return query(connection.get(), args, input) ? 0 : 1; //run the query with these args
    }

    //Otherwise if we are just starting the client then it should handle multiple commands hence we will setup the loop
    std::cout << "Commands: PING, ECHO, GET, SET, DEL, KEYS, ZADD, ZREM, ZSCORE, ZRANK, ZCARD, ZQUERY, EXPIRE, TTL, PERSIST.\n"
              << "See docs/COMMANDS.md for syntax. Type exit to leave.\n";
    std::string line;
    while (std::cout << "redis> " && std::getline(std::cin, line)) { //print redis> and wait for the user to enter a line and input and store that line in the line variable
        if (line == "exit") break; // Local UI action, not a server command.
        
        /*
        creates an input string stream named stream with the contents of the string line
        Why an input string strea? because it automatically separates words based on whitespace and moves the reading position once a word ahs been extracted
        So we wil extract the words from the stream and add it to the args vector
        */
        std::istringstream stream(line); 
        std::vector<std::string> args;
        bool valid = true;

        /*
        std::ws--> skips the leading whitespace
        stream.peek() checks the next character without consuming it
        hence we are skipping the whitespaces and checking that the next character is not eof
        */
        while (stream >> std::ws && stream.peek() != std::char_traits<char>::eof()) {
            std::string arg;
            // Double quotes group spaces; this is a small CLI, not a shell parser.
            if (!(stream >> std::quoted(arg))) { valid = false; break; } //std::quoted extracts arguments even double qutoed ones continaing spaces Eg-"Hello World" is extracted as one argume Hello World
            args.push_back(std::move(arg)); //point the next element of the argument vector to this stirng and free the arg pointer for the next string 
        }
        if (!valid) { std::cerr << "Unclosed quoted argument\n"; continue; }
        if (!args.empty() && !query(connection.get(), args, input)) return 1; //if the query failed return 1 and exit the main(query failing is node invlaide command etc-more like some network issue etc)
    }
}
