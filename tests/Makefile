srcs := basic.cpp
objs := $(patsubst %.cpp,%.test,$(srcs))
all: $(objs)
.PHONY: all clean

CPPFLAGS ?=
override CPPFLAGS += \
	-I../src/ \
	-DRSEQ_MUTEX_DEBUG \
	-g \
	-std=c++20

LDFLAGS ?=
override LDFLAGS += -L../src/ -lrseqmutex -lpthread

%.test: %.cpp ../src/librseqmutex.a ../src/rseqmutex.h
	$(CXX) $(CPPFLAGS) $< $(LDFLAGS) -I ../src/  -o $@ 

../src/librseqmutex.a:
	$(MAKE) -C ../src

clean:
	rm -f *.test
