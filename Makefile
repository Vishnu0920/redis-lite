CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -pthread -Icommon

.PHONY: all test clean
all: build/server build/client

build:
	mkdir -p build

build/server: server/main.cpp common/resp.cpp common/resp.h common/net.h | build
	$(CXX) $(CXXFLAGS) server/main.cpp common/resp.cpp -o $@

build/client: client/main.cpp common/resp.cpp common/resp.h common/net.h | build
	$(CXX) $(CXXFLAGS) client/main.cpp common/resp.cpp -o $@

build/protocol_test: tests/protocol_test.cpp common/resp.cpp common/resp.h | build
	$(CXX) $(CXXFLAGS) tests/protocol_test.cpp common/resp.cpp -o $@

test: all build/protocol_test
	./build/protocol_test
	python3 tests/integration.py

clean:
	rm -rf build
