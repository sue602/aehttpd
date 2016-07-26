EXT_SRC = strext.c trie.c json.c hash.c murmur3.c reallocarray.c list.c
AE_SRC = ae.c zmalloc.c anet.c
HTTP_SRC = http_parser.c
SERVER_SRC = main.c server.c
SRC = $(EXT_SRC) $(AE_SRC) $(HTTP_SRC) $(SERVER_SRC)

BIN = ../aehttpd

CFLAGS = 
DEBUG_CFLAGS = -DDEBUG -g

LDFLAGS = -lpthread -lz
DEBUG_LDFLAGS = -lpthread -lz

all:
	gcc ${SRC} -o ${BIN} ${CFLAGS} ${LDFLAGS} 

debug:
	gcc ${SRC} -o ${BIN} ${DEBUG_CFLAGS} ${DEBUG_LDFLAGS} 

clean:
	rm -f $(BIN)
