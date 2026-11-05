srcs := counter.cpp shared_ptr.cpp shared_ptr_single_kill.cpp
objs := $(patsubst %.cpp,%.bench,$(srcs))
all: $(objs)
.PHONY: all clean

CPPFLAGS ?=
override CPPFLAGS += \
	-I../src/ -I../examples/

LDFLAGS ?=
override LDFLAGS += -L../src/ -lrseqmutex -lpthread

%.bench: %.cpp ../src/librseqmutex.a ../src/rseqmutex.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -std=c++17 -DNDEBUG -I ../src/ -O2 -o $@

shared_ptr.bench: ../examples/percpu_shared_ptr.h
shared_ptr_single_kill.bench: ../examples/percpu_shared_ptr.h

../src/librseqmutex.a:
	$(MAKE) -C ../src

clean:
	rm -f *.bench

