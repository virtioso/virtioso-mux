CC = gcc

CFLAGS = -Wall -Wextra -Werror=missing-prototypes -Wswitch -Wformat
CFLAGS += -Wchar-subscripts -Wparentheses  -Wtrigraphs -Wpointer-arith
CFLAGS += -Wmissing-declarations -Wredundant-decls  -Wundef -Wmain
CFLAGS += -Wreturn-type -Wmultichar  -Wunused -Wmissing-braces -Werror
CFLAGS += -Wno-missing-field-initializers

CEXTRA = -pthread -std=gnu99
SOURCES = tcu_com.c

all: virtioso-mux

virtioso-mux: tcu_com.c
	$(CC) $(CFLAGS) $(CEXTRA) tcu_com.c -o virtioso-mux

clean:
	rm -f *.o
	rm -f virtioso-mux
