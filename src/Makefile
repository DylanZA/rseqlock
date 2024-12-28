CFLAGS ?=
srcs := rseq.c mutex.c
objs := $(patsubst %.c,%.o,$(srcs))
all: librseqmutex.a
.PHONY: all 

override CFLAGS += \
	-lpthread

%.o: %.c
	$(CC) $(CFLAGS) $(LXXFLAGS) -c -O2 -g -o $@ $<

librseqmutex.a: $(objs)
	rm -f librseqmutex.a
	$(AR) rcs librseqmutex.a $^

clean:
	rm -f *.so *.a
