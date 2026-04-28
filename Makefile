CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LIBS    = -lncurses

HEADERS_APFS = APFS.h btree.h gpt.h

all: leeAPFS leeArchivo navvis

leeAPFS: leeAPFS.c $(HEADERS_APFS)
	$(CC) $(CFLAGS) -o $@ leeAPFS.c $(LIBS)

leeArchivo: leeArchivo.c $(HEADERS_APFS)
	$(CC) $(CFLAGS) -o $@ leeArchivo.c

# navvis se refiere a NAME_MAX — en macOS no lo expone <dirent.h>; lo inyectamos.
navvis: navvis.c
	$(CC) $(CFLAGS) -DNAME_MAX=255 -o $@ navvis.c $(LIBS)

# El disco vive en el zip — descomprimirlo una sola vez.
DiscoAPFS.dmg: DiscoAPFS.zip
	unzip -o DiscoAPFS.zip

disco: DiscoAPFS.dmg

run: all disco
	./leeAPFS DiscoAPFS.dmg

.PHONY: all disco run clean
clean:
	rm -f leeAPFS leeArchivo navvis /tmp/leeAPFS_blob.bin
