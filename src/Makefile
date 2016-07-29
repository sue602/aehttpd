EXT_SRC = strext.c trie.c json.c hash.c murmur3.c reallocarray.c list.c
AE_SRC = ae.c zmalloc.c anet.c
HTTP_SRC = http_parser.c
SERVER_SRC = main.c server.c
STATIC_LIB = ../usr/lib/libhiredis.a
SRC = $(EXT_SRC) $(AE_SRC) $(HTTP_SRC) $(SERVER_SRC) $(STATIC_LIB)

BIN = ../aehttpd

CFLAGS = -I../usr/include
DEBUG_CFLAGS = -DDEBUG -g

LDFLAGS = -lpthread -lz -L../usr/lib 

all:
	gcc ${SRC} -o ${BIN} ${CFLAGS} ${LDFLAGS} 

debug:
	gcc ${SRC} -o ${BIN} ${DEBUG_CFLAGS} ${LDFLAGS} 

clean:
	rm -f $(BIN)
