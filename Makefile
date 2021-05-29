UV = /opt/homebrew/Cellar/libuv/1.41.0
OPTS = -I$(UV)/include -O3

debug: main
	./main

build/cirbuf.o: src/cirbuf.c
	$(CC) -o build/cirbuf.o src/cirbuf.c $(OPTS) -c

build/fifo.o: src/fifo.c
	$(CC) -o build/fifo.o src/fifo.c $(OPTS) -c

build/ucp.o: src/ucp.c
	$(CC) -o build/ucp.o src/ucp.c $(OPTS) -c

main: main.c build/ucp.o build/fifo.o build/cirbuf.o
	$(CC) -o main main.c build/ucp.o build/fifo.o build/cirbuf.o $(OPTS) -L$(UV)/lib -luv

$(shell mkdir -p build)
