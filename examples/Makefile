UNAME := $(shell uname)
UV = $(PWD)/deps/libuv
OPTS = -I$(UV)/include -O3
LINK_UV = -L$(UV)/lib -luv

ifeq ($(UNAME), Darwin)
LINK = $(LINK_UV)
else
LINK = $(LINK_UV) -Wl,-rpath=$(UV)/lib
endif

debug: main
	./main

deps/libuv:
	rm -rf deps/*
	cd deps && wget https://github.com/libuv/libuv/archive/refs/tags/v1.42.0.tar.gz
	cd deps && tar xzf v1.42.0.tar.gz
	cd deps/libuv-1.42.0 && ./autogen.sh && ./configure --prefix=$(PWD)/deps/libuv && make && make install
	rm -rf deps/v1.42.0.tar.gz deps/libuv-1.42.0

build/cirbuf.o: ../src/cirbuf.c deps/libuv
	$(CC) -o build/cirbuf.o ../src/cirbuf.c $(OPTS) -c

build/fifo.o: ../src/fifo.c deps/libuv
	$(CC) -o build/fifo.o ../src/fifo.c $(OPTS) -c

build/ucp.o: ../src/ucp.c deps/libuv
	$(CC) -o build/ucp.o ../src/ucp.c $(OPTS) -c

main: main.c build/ucp.o build/fifo.o build/cirbuf.o deps/libuv
	$(CC) -o main main.c build/ucp.o build/fifo.o build/cirbuf.o $(OPTS) $(LINK)

server: server.c build/ucp.o build/fifo.o build/cirbuf.o deps/libuv
	$(CC) -o server server.c build/ucp.o build/fifo.o build/cirbuf.o $(OPTS) $(LINK)

client: client.c build/ucp.o build/fifo.o build/cirbuf.o deps/libuv
	$(CC) -o client client.c build/ucp.o build/fifo.o build/cirbuf.o $(OPTS) $(LINK)

$(shell mkdir -p build deps)
