UNAME := $(shell uname)
UV = $(PWD)/deps/libuv
OPTS = -I$(UV)/include -O3
LINK_UV = -L$(UV)/lib -luv
OBJS = build/ucp.o build/fifo.o build/cirbuf.o build/utils.o
AR ?= ar

ifeq ($(UNAME), Darwin)
LINK = $(LINK_UV)
else
LINK = $(LINK_UV) -Wl,-rpath=$(UV)/lib
endif

all: build/ucp.o build/fifo.o build/cirbuf.o build/utils.o deps/libuv

examples: client server main

test: main
	./examples/main

deps/libuv:
	rm -rf deps/*
	cd deps && wget https://github.com/libuv/libuv/archive/refs/tags/v1.42.0.tar.gz
	cd deps && tar xzf v1.42.0.tar.gz
	cd deps/libuv-1.42.0 && ./autogen.sh && ./configure --prefix=$(PWD)/deps/libuv && make && make install
	rm -rf deps/v1.42.0.tar.gz deps/libuv-1.42.0

build/utils.o: ./src/utils.c
	$(CC) -o build/utils.o ./src/utils.c $(OPTS) -c

build/cirbuf.o: ./src/cirbuf.c deps/libuv
	$(CC) -o build/cirbuf.o ./src/cirbuf.c $(OPTS) -c

build/fifo.o: ./src/fifo.c deps/libuv
	$(CC) -o build/fifo.o ./src/fifo.c $(OPTS) -c

build/ucp.o: ./src/ucp.c deps/libuv
	$(CC) -o build/ucp.o ./src/ucp.c $(OPTS) -c

libucp.a: $(OBJS)
	$(AR) rvs build/libucp.a $(OBJS)

main: $(OBJS) deps/libuv
	$(CC) -o examples/main examples/main.c $(OBJS) $(OPTS) $(LINK)

server: $(OBJS) deps/libuv
	$(CC) -o examples/server examples/server.c $(OBJS) $(OPTS) $(LINK)

client: $(OBJS) deps/libuv
	$(CC) -o examples/client examples/client.c $(OBJS) $(OPTS) $(LINK)

clean:
	rm -rf build examples/main examples/server examples/client

$(shell mkdir -p build deps)
