CFLAGS=-Os -Wall -Wno-unused-parameter -Wextra -Wwrite-strings

ALL=nitro nitroctl

all: $(ALL)

debug: CFLAGS+=-g -Og -DDEBUG
debug: $(ALL)

tiny: CFLAGS=-Os -Wl,--gc-sections -fno-asynchronous-unwind-tables -fno-stack-protector -fno-stack-clash-protection
tiny: LDFLAGS=-static
tiny: $(ALL)

clean: FRC
	rm -f $(ALL)

release:
	VERSION=$$(git describe --tags | sed 's/^v//;s/-[^.]*$$//') && \
	git archive --prefix=nitro-$$VERSION/ -o nitro-$$VERSION.tar.gz HEAD

sign:
	VERSION=$$(git describe --tags | sed 's/^v//;s/-[^.]*$$//') && \
	gpg2 --armor --detach-sign nitro-$$VERSION.tar.gz && \
	signify -S -s ~/.signify/nitro.sec -m nitro-$$VERSION.tar.gz && \
	sed -i '1cuntrusted comment: verify with nitro.pub' nitro-$$VERSION.tar.gz.sig

FRC:

TESTCASES != printf '%s\n' t/[0-9]*.rb | sed 's/rb$$/FRC/'
.SUFFIXES: .rb .FRC

check: t.out $(TESTCASES)

t.out:
	mkdir -p t.out

.rb.FRC:
	@ruby $< > t.out/$$(basename $< .rb).out 2>&1 && echo "ok $<" || { echo "not ok $<"; exit 1; }
