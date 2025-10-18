CFLAGS=-Os -Wall -Wno-unused-parameter -Wextra -Wwrite-strings -Wno-string-plus-int

ALL=nitro nitroctl

all: $(ALL)

nitro: nitro.c nitro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

nitroctl: nitroctl.c nitro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -lm

debug:
	$(MAKE) all CFLAGS="$(CFLAGS) -g -Og -DDEBUG -D_FORTIFY_SOURCE=2"

tiny:
	$(MAKE) all CFLAGS="-Os -Wl,--gc-sections -fno-stack-protector \
		-fno-stack-clash-protection -fno-asynchronous-unwind-tables" \
		LDFLAGS="-static"

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

check: t.out $(TESTCASES)

t.out:
	mkdir -p t.out

.SUFFIXES: .rb .FRC

.rb.FRC:
	@ruby $< > t.out/$$(basename $< .rb).out 2>&1 && echo "ok $<" || { echo "not ok $<"; exit 1; }
