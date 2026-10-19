#pragma once

#include <memory>
#include "timers/time.h"
class CommandProcessor;
enum class InputBufferMode { Regular, Offset };

// The caller owns the listener. Both modes make it nonblocking; threaded workers
// still use blocking client sockets (Linux accept does not inherit O_NONBLOCK).
int runThreadedServer(int listener, std::shared_ptr<CommandProcessor> commands);
int runEventLoopServer(int listener, std::shared_ptr<CommandProcessor> commands,
                       InputBufferMode bufferMode = InputBufferMode::Offset,
                       std::uint64_t idleTimeoutMS = timers::DefaultIdleTimeoutMS);
