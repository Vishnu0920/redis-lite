CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -pthread -Icommon -Iserver
COMMAND_SOURCES = server/commands/commands.cpp
COMMAND_HEADERS = server/commands/commands.h
STORAGE_SOURCES = server/storage/storage.cpp
TIMER_HEADERS = server/timers/time.h server/timers/idle_list.h
STORAGE_HEADERS = server/storage/storage.h server/storage/data_structures/hash_table.h server/storage/data_structures/avl_tree.h server/storage/data_structures/min_heap.h $(TIMER_HEADERS)
RUNNER_SOURCES = server/runners/threaded.cpp server/runners/event_loop.cpp
RUNNER_HEADERS = server/runners/runners.h server/runners/connection_log.h server/buffers/input_buffer.h
SERVER_SOURCES = server/main.cpp $(RUNNER_SOURCES) $(COMMAND_SOURCES) $(STORAGE_SOURCES)
SERVER_HEADERS = $(RUNNER_HEADERS) $(COMMAND_HEADERS) $(STORAGE_HEADERS)

.PHONY: all test clean
all: build/server build/client

build:
	mkdir -p build

build/server: $(SERVER_SOURCES) $(SERVER_HEADERS) common/resp.cpp common/resp.h common/net.h | build
	$(CXX) $(CXXFLAGS) $(SERVER_SOURCES) common/resp.cpp -o $@

build/client: client/main.cpp common/resp.cpp common/resp.h common/net.h | build
	$(CXX) $(CXXFLAGS) client/main.cpp common/resp.cpp -o $@

build/protocol_test: tests/protocol_test.cpp common/resp.cpp common/resp.h | build
	$(CXX) $(CXXFLAGS) tests/protocol_test.cpp common/resp.cpp -o $@

build/input_buffer_test: tests/input_buffer_test.cpp server/buffers/input_buffer.h common/resp.cpp common/resp.h | build
	$(CXX) $(CXXFLAGS) tests/input_buffer_test.cpp common/resp.cpp -o $@

build/storage_test: tests/storage_test.cpp $(STORAGE_SOURCES) $(STORAGE_HEADERS) | build
	$(CXX) $(CXXFLAGS) tests/storage_test.cpp $(STORAGE_SOURCES) -o $@

build/commands_test: tests/commands_test.cpp $(COMMAND_SOURCES) $(COMMAND_HEADERS) $(STORAGE_SOURCES) $(STORAGE_HEADERS) common/resp.cpp common/resp.h | build
	$(CXX) $(CXXFLAGS) tests/commands_test.cpp $(COMMAND_SOURCES) $(STORAGE_SOURCES) common/resp.cpp -o $@

build/timers_test: tests/timers_test.cpp $(STORAGE_SOURCES) $(STORAGE_HEADERS) | build
	$(CXX) $(CXXFLAGS) tests/timers_test.cpp $(STORAGE_SOURCES) -o $@

test: all build/protocol_test build/input_buffer_test build/storage_test build/commands_test build/timers_test
	./build/protocol_test
	./build/input_buffer_test
	./build/storage_test
	./build/commands_test
	./build/timers_test
	python3 tests/integration.py

clean:
	rm -rf build
