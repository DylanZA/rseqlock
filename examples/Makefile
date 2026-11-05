all: shared_ptr.example
.PHONY: all clean

CPPFLAGS += -I../src
CXXFLAGS ?= -O2 -g

shared_ptr.example: shared_ptr.cpp percpu_shared_ptr.h ../src/rseqmutex.h ../src/librseqmutex.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 $< $(LDFLAGS) -L../src -lrseqmutex -pthread -o $@

../src/librseqmutex.a:
	$(MAKE) -C ../src

clean:
	rm -f *.example
