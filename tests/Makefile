srcs := basic.cpp shared_ptr.cpp
objs := $(patsubst %.cpp,%.test,$(srcs))
all: $(objs)
.PHONY: all clean check-shared-ptr

CPPFLAGS ?=
override CPPFLAGS += \
	-I../src/ \
	-I../examples/ \
	-DRSEQ_MUTEX_DEBUG \
	-g \
	-std=c++20

LDFLAGS ?=
override LDFLAGS += -L../src/ -lrseqmutex -lpthread

%.test: %.cpp ../src/librseqmutex.a ../src/rseqmutex.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@

shared_ptr.test: ../examples/percpu_shared_ptr.h
shared_ptr.test: LDFLAGS += -Wl,--wrap=rseq_mutex_spin_lock_cpu -Wl,--wrap=rseq_mutex_spin_unlock_cpu

check-shared-ptr: shared_ptr.test
	./shared_ptr.test

../src/librseqmutex.a:
	$(MAKE) -C ../src

clean:
	rm -f *.test
