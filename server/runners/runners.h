#pragma once

#include <memory>
class CommandProcessor;
enum class InputBufferMode { Regular, Offset };

// The caller owns the listener. Event-loop mode makes it nonblocking.
int runThreadedServer(int listener, std::shared_ptr<CommandProcessor> commands);
int runEventLoopServer(int listener, std::shared_ptr<CommandProcessor> commands,
                       InputBufferMode bufferMode = InputBufferMode::Offset);
