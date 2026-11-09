CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
BATS := ../bats
SRCS := bats_live.c $(BATS)/field.c $(BATS)/source.c $(BATS)/dest.c $(BATS)/codec.c

ifeq ($(OS),Windows_NT)
  LIBS := -lws2_32
else
  LIBS :=
endif

.PHONY: all test clean

all: bats_live

bats_live: $(SRCS) psi_live.h $(BATS)/bats_codec.h
	$(CC) $(CFLAGS) -I$(BATS) -o bats_live $(SRCS) $(LIBS)

test: bats_live
	./bats_live --role selftest

clean:
	rm -f bats_live bats_live.exe
