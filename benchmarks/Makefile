srcs := counter.cpp
objs := $(patsubst %.cpp,%.bench,$(srcs))
all: $(objs)
.PHONY: all

CPPFLAGS ?=
override CPPFLAGS += \
	-I../src/

LDFLAGS ?=
override LDFLAGS += -L../src/ -lrseqmutex -lpthread

%.bench: %.cpp ../src/librseqmutex.a ../src/rseqmutex.h
	$(CXX) $(CPPFLAGS) $< $(LDFLAGS) -I ../src/ -O2  -o $@ 
clean:
	rm -f *.bench

