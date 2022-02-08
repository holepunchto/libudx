CFLAGS += -Iinclude

libudx.a: libudx.a(src/cirbuf.o src/fifo.o src/udx.o src/utils.o)

src/cirbuf.o: include/udx/cirbuf.h
src/fifo.o: include/udx/fifo.h
src/udx.o: include/udx.h
src/utils.o: include/udx/utils.h

examples/main: libudx.a examples/main.c -ludx -luv
examples/client: libudx.a examples/client.c -ludx -luv
examples/server: libudx.a examples/server.c -ludx -luv
