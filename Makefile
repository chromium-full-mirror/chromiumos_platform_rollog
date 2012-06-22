#
# Initial simple Makefile for rollog, to be battered into shape later.
#
PROG=	rollog
ARCH=	-m32

CFLAGS+= -I. $(ARCH) -static
CFLAGS+= -Wall -Werror
SRCS=	main.c
OBJS=	$(SRCS:.c=.o)

all: $(PROG)

$(OBJS): $(SRCS)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -f $(PROG) $(OBJS)

