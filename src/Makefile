CFLAGS ?=
srcs := rseq.c mutex.c
objs := $(patsubst %.c,%.o,$(srcs))
all: librseqmutex.a
.PHONY: all clean

%.o: %.c rseqmutex.h
	$(CC) $(CFLAGS) $(LXXFLAGS) -c -O2 -g -o $@ $<

librseqmutex.a: $(objs)
	rm -f librseqmutex.a
	$(AR) rcs librseqmutex.a $^

clean:
	rm -f *.o *.so *.a
