CC=gcc
CFLAGS=-fPIC -Wall -Wextra -O3 -DNDEBUG
LDFLAGS=-shared

TARGET_LIB=libzicio.so

SRCS=zicio_lib.c # source files
OBJS=$(SRCS:.c=.o)
SUBDIRS=tests # tests directory

.PHONY: all
all: $(TARGET_LIB)

$(TARGET_LIB): $(OBJS)
	$(CC) ${LDFLAGS} -o $@ $^

$(OBJS): $(SRCS)
	$(CC) ${CFLAGS} -c ${SRCS}

test: $(SUBDIRS)
	make clean
	make all
	sudo cp ./libzicio.so /usr/lib
	sudo cp ./include/libzicio.h /usr/include
	$(MAKE) -C tests

# compile test
#test: $(TARGET_LIB)
#	$(CC) ${CFLAGS} -I./include -L. -o test test.c -lzicio -lpthread

.PHONY: clean
clean:
	rm -f ${TARGET_LIB} ${OBJS} test.o test
	$(MAKE) clean -C ${SUBDIRS} 
