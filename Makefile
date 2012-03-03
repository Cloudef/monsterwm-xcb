# Makefile for monsterwm - see LICENSE for license and copyright information

VERSION = xcb-cloudef-personal-git
WMNAME  = monsterwm

PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin
MANPREFIX = ${PREFIX}/share/man

INCS = -I. -I/usr/include
LIBS = -L/usr/lib -lc -lX11-xcb `pkg-config --libs gl x11 xcb-glx xcb-composite xcb-icccm xcb-keysyms cairo pangocairo`

CPPFLAGS = -DVERSION=\"${VERSION}\" -DWMNAME=\"${WMNAME}\"
CFLAGS   = -std=c99 -pedantic -Wall -Wextra ${INCS} ${CPPFLAGS} `pkg-config --cflags xcb cairo pangocairo`
LDFLAGS  = ${LIBS}

XINERAMA = 1
DEBUG	 = 1

CC 	 = cc
EXEC = ${WMNAME}

SRC = ${WMNAME}.c GL/glcomposite.c
OBJ = ${SRC:.c=.o}

ifeq (${DEBUG},0)
   CFLAGS  += -Os
   LDFLAGS += -s
else
   CFLAGS  += -g
   LDFLAGS += -g
endif

ifeq (${XINERAMA},1)
   LDFLAGS += -lxcb-xinerama
   CFLAGS  += -DXINERAMA=1
endif

all: options ${WMNAME}

options:
	@echo ${WMNAME} build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

%.o: %.c
	@echo CC $<
	@${CC} -c -o $@ ${CFLAGS} $<

${OBJ}: config.h

config.h:
	@echo creating $@ from config.def.h
	@cp config.def.h $@

${WMNAME}: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	@echo cleaning
	@rm -fv ${WMNAME} ${OBJ} ${WMNAME}-${VERSION}.tar.gz

install: all
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@install -Dm755 ${WMNAME} ${DESTDIR}${PREFIX}/bin/${WMNAME}
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man.1
	@install -Dm644 ${WMNAME}.1 ${DESTDIR}${MANPREFIX}/man1/${WMNAME}.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/${WMNAME}
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/${WMNAME}.1

.PHONY: all options clean install uninstall
