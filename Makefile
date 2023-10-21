CFLAGS=-g -O2 -Wall -Wno-switch -Wextra -Wwrite-strings
LDLIBS=-luv

ALL=nitro

all: $(ALL)

clean: FRC
	rm -f $(ALL)

FRC:
