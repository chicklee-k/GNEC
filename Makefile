CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
LDLIBS  = -lm

# EUC-KR 입력 변환에 POSIX iconv 를 쓴다. 비활성화하려면: make NO_ICONV=1
ifdef NO_ICONV
CFLAGS += -DGNE_NO_ICONV
endif

SRC = src/lexer.c src/parser.c src/value.c src/loader.c src/interp.c src/main.c
OBJ = $(SRC:.c=.o)
BIN = gnec

.PHONY: all clean test debug
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/gnec.h src/keywords.h
	$(CC) $(CFLAGS) -c -o $@ $<

debug: CFLAGS = -std=c11 -g -O0 -Wall -Wextra -Wpedantic -fsanitize=address,undefined
debug: clean $(BIN)

test: $(BIN)
	@sh tests/run_tests.sh ./$(BIN)

clean:
	rm -f $(OBJ) $(BIN)
