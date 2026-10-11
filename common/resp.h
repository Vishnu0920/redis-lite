#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
The entire code here is placed within a namespace block that allows us to  group simiar entities/names.
Prevents collisons for similarly named functions if used anywhere else
Eg- resp::parseCommand(input);
*/
namespace resp {
/*
First we will define limits for the commands, arguments etc
We use constexpr to make it compile time constants
*/
constexpr std::size_t MaxBulk = 64 * 1024; //One argument can contain at most 64kb(argument--> eg SET name vale ie the SET command has 2 arguments)
constexpr std::size_t MaxFrame = 1024 * 1024;//One encoded request can be atmost 1mb-->encoded request is the stream after converting the full command +arguments into RESP format
//Eg-*3/$3/SET.....so on - this is one encoded request

constexpr std::size_t MaxArgs = 128; //The full cmmand string can contain atmost 128 elements including command names + arguments
//Eg- DEL key1 key2 key3  ..... max no of elemtns including the DEL word is 128




/*
Enum to store the status of the parser
Complete  -->One entire message has been decoded and now we can use the bytes and then remove them
Incomplete-->The abailable bytes might form a valid message but more are needed. Retain the bytes and recieve more
Error     -->The bytes recieved cant fomr a valid message. So report an error and close the connection
*/
enum class Status { Complete, Incomplete, Error };


/*
The server side result after parsing is complete is stored in this struct.
By default the status is incomplete
We also have a vector of strings to store the arguments for each command
consumed-->the input buffer may contain byted from more than 1 command. hence to clelarly differentiate and remove only the bytes needed for 
current command we need to track the bytes consumed
error-->if there is any error it will hld the error and description
*/
struct CommandResult {
    Status status = Status::Incomplete;
    std::vector<std::string> arguments;
    std::size_t consumed = 0;
    std::string error;
};


/*
This is the client side result(this is how the response from the server is stored client side)
type-->RESP2 prefix: + simple, - error, $ bulk/null, : integer, * array/null
consumed-->again smae thing here- the bytes may contain bytes form a different response as well hence to clealry identify when the current response is over
*/
struct ReplyResult {
    Status status = Status::Incomplete;
    char type = 0;
    std::string value;
    std::size_t consumed = 0;
    std::string error;
    bool isNull = false;
    std::int64_t integer = 0;
    std::vector<ReplyResult> elements;
};

//Hwere we are declaring functions. The imlemetatiosn will be in the .cpp file 
std::string encodeCommand(const std::vector<std::string>& arguments);
std::string simple(std::string_view value);
std::string error(std::string_view value);
std::string bulk(std::string_view value);
std::string nullBulk();
std::string integer(std::int64_t value);
// Elements are already encoded RESP replies, allowing arrays of mixed types.
std::string array(const std::vector<std::string>& encodedElements);
CommandResult parseCommand(std::string_view input);
// Decodes RESP2 simple/error strings, bulk strings, nulls, integers, and arrays.
ReplyResult parseReply(std::string_view input);
}
