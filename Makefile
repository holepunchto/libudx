libudx.a: libudx.a(src/cirbuf.o src/fifo.o src/udx.o src/utils.o)

src/cirbuf.o: include/udx/cirbuf.h
src/fifo.o: include/udx/fifo.h
src/udx.o: include/udx.h
src/utils.o: include/udx/utils.h

examples/main: examples/main.c -ludx -luv
examples/client: examples/client.c -ludx -luv
examples/server: examples/server.c -ludx -luv
