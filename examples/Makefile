UV = $(PWD)/deps/libuv
OPTS = -I$(UV)/include -O3

debug: main
	./main

deps/libuv:
	rm -rf deps/*
	cd deps && wget https://github.com/libuv/libuv/archive/refs/tags/v1.42.0.tar.gz
	cd deps && tar xzf v1.42.0.tar.gz
	cd deps/libuv-1.42.0 && ./autogen.sh && ./configure --prefix=$(PWD)/deps/libuv && make && make install
	rm -rf deps/v1.42.0.tar.gz deps/libuv-1.42.0

build/cirbuf.o: ../src/cirbuf.c
	$(CC) -o build/cirbuf.o ../src/cirbuf.c $(OPTS) -c

build/fifo.o: ../src/fifo.c
	$(CC) -o build/fifo.o ../src/fifo.c $(OPTS) -c

build/ucp.o: ../src/ucp.c
	$(CC) -o build/ucp.o ../src/ucp.c $(OPTS) -c

main: main.c build/ucp.o build/fifo.o build/cirbuf.o deps/libuv
	$(CC) -o main main.c build/ucp.o build/fifo.o build/cirbuf.o $(OPTS) -L$(UV)/lib -luv

server: server.c build/ucp.o build/fifo.o build/cirbuf.o
	$(CC) -o server server.c build/ucp.o build/fifo.o build/cirbuf.o $(OPTS) -L$(UV)/lib -luv

client: client.c build/ucp.o build/fifo.o build/cirbuf.o
	$(CC) -o client client.c build/ucp.o build/fifo.o build/cirbuf.o $(OPTS) -L$(UV)/lib -luv

$(shell mkdir -p build deps)
