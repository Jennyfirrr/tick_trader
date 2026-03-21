CXX      = g++
CXXFLAGS = -std=c++17 -O2
LIBS     = -lssl -lcrypto

# default: multicore TUI production build
all: engine

engine:
	$(CXX) $(CXXFLAGS) -DMULTICORE_TUI $(LIBS) -lpthread -o engine main.cpp

# profiling builds
profile:
	$(CXX) $(CXXFLAGS) -DLATENCY_PROFILING -DMULTICORE_TUI $(LIBS) -lpthread -o engine main.cpp

profile-lite:
	$(CXX) $(CXXFLAGS) -DLATENCY_PROFILING -DLATENCY_LITE -DMULTICORE_TUI $(LIBS) -lpthread -o engine main.cpp

# bench: profiling with TUI disabled, stats to stderr
bench:
	$(CXX) $(CXXFLAGS) -DLATENCY_PROFILING -DLATENCY_BENCH $(LIBS) -o engine main.cpp

# single-threaded (no multicore TUI)
single:
	$(CXX) $(CXXFLAGS) $(LIBS) -o engine main.cpp

# tests
test:
	$(CXX) $(CXXFLAGS) -I.. -o tests/controller_test tests/controller_test.cpp
	./tests/controller_test

clean:
	rm -f engine engine_st tests/controller_test

.PHONY: all engine profile profile-lite bench single test clean
