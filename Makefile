CC	:= gcc
CFLAGS	:= -DINTEL -Wall -std=c99
LDFLAGS	:= -lpthread -lm

OS	:= $(shell uname -s)
    ifeq ($(OS),Linux)
	CFLAGS  += -DCACHE_LINE_SIZE=`getconf LEVEL1_DCACHE_LINESIZE`
        LDFLAGS += -lrt
    endif
    ifeq ($(OS),Darwin)
	CFLAGS += -DCACHE_LINE_SIZE=`sysctl -n hw.cachelinesize`
    endif

ifeq ($(DEBUG),true)
	CFLAGS+=-DDEBUG -O0 -ggdb3 #-fno-omit-frame-pointer -fsanitize=address
else
	CFLAGS+=-O3
endif


VPATH	:= gc
DEPS	+= Makefile $(wildcard *.h) $(wildcard gc/*.h)

TARGETS := perf_meas unittests


all:	$(TARGETS)

clean:
	rm -f $(TARGETS) core *.o test

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

perf_meas: CFLAGS+=-DNDEBUG
$(TARGETS): %: %.o ptst.o gc.o prioq.o common.o
	$(CC) -o $@ $^ $(LDFLAGS)

unittest: unittests
	./unittests

test:
	make ptst.o gc.o prioq.o common.o
	g++ -O3 -pthread test.cpp ptst.o gc.o prioq.o common.o -lm -lrt -o test

.PHONY: all clean test
