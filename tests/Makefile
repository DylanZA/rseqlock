srcs := basic.cpp
objs := $(patsubst %.cpp,%.test,$(srcs))
all: $(objs)
.PHONY: all

CPPFLAGS ?=
override CPPFLAGS += \
	-I../src/ \
	-std=c++20

LDFLAGS ?=
override LDFLAGS += -L../src/ -lrseqmutex -lpthread

%.test: %.cpp ../src/librseqmutex.a ../src/rseqmutex.h
	$(CXX) $(CPPFLAGS) $< $(LDFLAGS) -I ../src/ -O2  -o $@ 
clean:
	rm -f *.test

