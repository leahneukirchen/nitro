CFLAGS=-g -O2 -Wall -Wno-unused-parameter -Wextra -Wwrite-strings

ALL=nitro nitroctl

all: $(ALL)

clean: FRC
	rm -f $(ALL)

FRC:
