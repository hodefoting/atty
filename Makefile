DESTDIR ?=
PREFIX  ?= /usr/local
CFLAGS  += -O3 `pkg-config --cflags sdl2`
CFLAGS  += -Wall -Wextra -Wno-unused-parameter
LDLIBS = -lutil -lz `pkg-config --libs sdl2`
all: atty
atty: atty.c
	$(CC) $(CFLAGS) *.c -o atty $(LDLIBS)
install: atty
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m755 atty $(DESTIRT)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/man
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m644 atty.1 $(DESTIRT)$(PREFIX)/share/man/man1
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/atty
clean:
	rm atty -f
