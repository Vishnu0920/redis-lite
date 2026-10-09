#include "resp.h"
#include <utility>

namespace resp {//This namespace is named namespace used for avoiding collisions with function/entity names
namespace {//This namespace is anonymous namespace that tells that everything within this block is private to this cpp file
struct Reader {
    /*
    string_view is a data type where the object points directly at the string. 
    when we pass a normal string to a function the function gets a new copy of the string(unless we pass by reference)
    here we can just pass the string_view and it will point to the same string
    string_view is also used only for aread only actions like the one we are doin here
    We dont use passing string be reference here because it is more expensive(requires copying the string from one place to another)
    We just want to look at the string
        TCP buffer
        +------------------------------------------------+
        | *2\r\n$3\r\nGET\r\n$5\r\nhello\r\n           |
        +------------------------------------------------+
            ^
            |
            string_view
    */
    std::string_view input;
    std::size_t pos = 0; //Keeps track of the location where the reader is currently at
    Status status = Status::Complete;
    std::string message; //if there is an error this is what stores the error description

    /*
    funnction that sets the reader structs field values when parsing fails
    takes a char pointer(baiscally string) and based on that stores the error message
    it returns false-because the calling functions are need() and marker() and they need to return false if a message is invalid
    */
    bool fail(const char* why) {
        status = Status::Error;
        message = why;
        return false;
    }


    //It checks whether there are count number of more bytes in the buffer/input to form the encoded request
    bool need(std::size_t count) {
        //can also be written as count + pos > MaxFrame ->but to avoid integer overflow
        if (count > MaxFrame - pos) return fail("frame too large"); //This protects against messages that exceed the max lenght limit
        
        /*
        basically count+pos represents the total bytes needed for the current command being parsed
        count is how many more bytes have to be parsed for current command
        pos is the no of bytes already parsed in the curent command
        */
        if (count > input.size() - pos) { //checs whether the currently recieved buffer contains enough bytes-if not it means that this command's bytes have not yet fully arrived, hence Incomplete
            status = Status::Incomplete;
            return false;
        }
        return true;
    }



    /*
    This checks whetehr the current byte is what we expect
    For eg-commands should start with *. If the current byte is not * then we should return false
    if we call marker(*)-->it measn we are checking whether the current read locations's byte matches with the *
    */
    bool marker(char expected) {
        if (!need(1)) return false; //We first check whether we can read from the buffer.input for the current command
        if (input[pos] != expected) return fail("unexpected RESP type");
        ++pos;
        return true;
    }

    /*
    This is the function that parses the RESP length numbers
    Eg- in $5\r\nhello\r\n it parses 5 and tells the reader that the string coming next will be 5 characters
    It stores the actual value in the value variable which is passed as reference
    */
    bool number(std::size_t limit, std::size_t& value) {//limit here is the max numeric value allowed->we usually pass MaxArgs ie the max number of arguments allowed or MaxBulk
        value = 0; //Initially we say the value is 0 - we will accumulate the actual number into this
        std::size_t digits = 0; //this is to keep track of the number of digits in the current number
        
        while (true) {
            if (!need(1)) return false; //if the current command doesnt need atleast 1 more character it means the command is already completed hence for this command the number is not needed. return false
            const char ch = input[pos];
            if (ch == '\r') {//When we reach the final position the \r part of the \r\n terminator
                if (digits == 0) return fail("empty length"); //if the no of digits=0 that means the number is empty
                if (!need(2)) return false; //checks if 2 bytes are available ie \r and \n if not that means the message might not have fully arrived yet, but current number operation will return false(but not fail)
                if (input[pos + 1] != '\n') return fail("expected CRLF");//here we chack if the next byte is /n --> if not then immediately fail this comands reading(here fail-which also returns false)
                pos += 2; //Move the pos past the /r and /n
                return true; //we have successfully parsed the number so return true
            }
            
            if (ch < '0' || ch > '9') return fail("invalid length"); //Validate that all characters are digits
            if (++digits > 20) return fail("length header too long"); //We check whether the number of digits is too long
            const std::size_t digit = static_cast<std::size_t>(ch - '0');//Convert char digit to int
            if (value > limit / 10 ||
                (value == limit / 10 && digit > limit % 10)) { //All this does is endure that value doesnt cross the limit
                return fail("length exceeds limit");
            }
            value = value * 10 + digit; //Accumulate the number in value
            ++pos;
        }
    }


    /*
    This parses a bulk string
    Eg- $5\r\nhello\r\n --> after parsing the length will be 5 and the string will be hello
    */
    bool readBulk(std::string& value) {//value is the string that will hold the final bulk string that is parsed
        std::size_t length = 0; //the length of the bulk string will be save in this
        if (!marker('$') || !number(MaxBulk, length)) return false; //Check whether the marker is correct for bulk string and parse the lenght number
        if (!need(length + 2)) return false; //Check whether the entire string + the \r\n is present in the input-else return false for now
        if (input[pos + length] != '\r' || input[pos + length + 1] != '\n') { //Check whether the terminator is correct \r\n
            return fail("missing bulk string terminator");
        }
        value.assign(input.substr(pos, length)); //copies the value in the string_view object input fromm the current position to current position + length
        pos += length + 2;
        return true;
    }
};
}//The anonymous namespace block ends here - ie everything abpve this(the Reader struct is private)
//The functions below this are public and this is what the other files can use for the parsing

/*
This converts the string to be sent into RESP format
It adds the $ + length of string + the string itself + \r\n
It sends responses in bulk string format
*/
std::string bulk(std::string_view value) {
    return "$" + std::to_string(value.size()) + "\r\n" +
           std::string(value) + "\r\n";
}


/*
This function creates a simple string. Prefixes the +
Simple string is sent for simple onw word responses like PONG, OK etc ie responses that dont need byte length to be parsed
*/
std::string simple(std::string_view value) {
    return "+" + std::string(value) + "\r\n";
}


/*
This creates an error string. Prefixes the -
*/
std::string error(std::string_view value) {
    return "-" + std::string(value) + "\r\n";
}

/*
This creates the RESP string for a full command. Preifxes *, adds the number of arguments + The arguments
*/
std::string encodeCommand(const std::vector<std::string>& arguments) {//arguments is a vector which might look like this: {"SET", "value", "10"}
    std::string result = "*" + std::to_string(arguments.size()) + "\r\n"; //Addint the * prefix and the length
    for (const auto& argument : arguments) result += bulk(argument); //Here we are adding each argument as an individual bulk string
    return result;
}


/*
This decodes the RESP string of a command and converts it into an vector of arguments
*/
CommandResult parseCommand(std::string_view input) { //return type of this is the commandResult object
    Reader reader{input, 0, Status::Complete, {}}; //The reader object is initalised to parse the command
    CommandResult result; //the result object which will be returned
    
    /*
    A lambda function that stores the value of an unfinished/error command.
    We first put the status of the reader into the the reuslt object and clear the arguments(because 
    some arguments may have been parsed correctly but the others might not have). hence we flush/clear all arguments
    */
    auto unfinished = [&]() {
        result.status = reader.status;
        result.error = reader.message;
        result.arguments.clear();
        return result;
    };

    std::size_t count = 0; //stores the count of arguments ie the command name + argument
    if (!reader.marker('*') || !reader.number(MaxArgs, count)) return unfinished(); //Check for correct marker and then parse the number of arguments
    if (count == 0) {//if no of arguments is 0 return failed
        reader.fail("empty command array");//whenever we call fail anywhere it also sets the status to error
        return unfinished(); //when we fail a command read return the unfinished result object.(Technically call the unfinished lambda function that returns the result object)
    }

    for (std::size_t i = 0; i < count; ++i) {//Read the arguments/parse the arguments(using readBulk) and store them in the arguments vector
        std::string argument;
        if (!reader.readBulk(argument)) return unfinished();//if the argument is not parsed correctly let the readBulk function handle the status values, jsut return the unfinished object after that
        result.arguments.push_back(std::move(argument)); //Here we are usign move() so that instead of creating a new copy of string in the memory, then pointing the arguments[i]th index at it and then freeing argumnets, this directly points arguments[i] to the location of the current string itslef and automatically frees argument variable also(much cheaper and faster)
    }
    result.status = Status::Complete; //once all arguments are parsed set status as complete
    result.consumed = reader.pos; //set the no of bytes in this command, so that the caller of this function knows how many bytes to remove from the input stream to start parsing the next command
    return result; //return the fully finished result object
}


/*
This parses the responses from the server 
*/
ReplyResult parseReply(std::string_view input) {
    ReplyResult result;
    if (input.empty()) return result;
    result.type = input[0];//The first char tells the type of result
    if (result.type == '$') {// $ means bulk string
        Reader reader{input, 0, Status::Complete, {}};
        if (!reader.readBulk(result.value)) { //parse the bulk string
            result.status = reader.status;
            result.error = reader.message;
            return result;
        }
        result.consumed = reader.pos;
    } 
    else if (result.type == '+' || result.type == '-') {
        const auto end = input.find("\r\n", 1); //search for the \r\n substring starting from pos 1(pos 1 becasue 0 contains + or -)
        if (end == std::string_view::npos) {//npos means not found-->therefore if the \r\n wasnt found then it is possible that the reply is just incomplete
            if (input.size() >= MaxFrame) {//but if the inout size is already greater than the MaxFram size then it is invalid
                result.status = Status::Error;
                result.error = "reply too large";
            }
            return result; //this occurs on every call-here we dont set the status to error of input size>maxFram-so by defualt this sends the result with status incomplete
        }
        if (end + 2 > MaxFrame) { //check if the total length is less than the MaxFrame size
            result.status = Status::Error;
            result.error = "invalid reply line";
            return result;
        }
        result.value = std::string(input.substr(1, end - 1));
        result.consumed = end + 2;
    } 
    
    /*
    If any other character type for the first elemetn then it is not deifned and hence error
    */
    else {
        result.status = Status::Error;
        result.error = "unsupported reply type";
        return result;
    }

    //Finally if we reach here successuly then the reply has been recieved and parsed successfully
    result.status = Status::Complete;
    return result;
}
}
