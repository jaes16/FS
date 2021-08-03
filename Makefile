

# Root for OSXFUSE includes and libraries
OSXFUSE_ROOT = /usr/local
#OSXFUSE_ROOT = /opt/local

INCLUDE_DIR = $(OSXFUSE_ROOT)/include/osxfuse/fuse
LIBRARY_DIR = $(OSXFUSE_ROOT)/lib

CC ?= gcc

CFLAGS_OSXFUSE = -I$(INCLUDE_DIR) -L$(LIBRARY_DIR)
CFLAGS_OSXFUSE += -DFUSE_USE_VERSION=26
CFLAGS_OSXFUSE += -D_FILE_OFFSET_BITS=64
CFLAGS_OSXFUSE += -D_DARWIN_USE_64_BIT_INODE

CFLAGS_EXTRA = -Wall -g $(CFLAGS)

LIBS = -lfuse

build/fs: src/fs.c src/fs_helper.c
	gcc $(CFLAGS_OSXFUSE) $(CFLAGS_EXTRA) src/fs.c src/fs_helper.c -o build/fs $(LIBS)

#%.o: %.c
#	$(CC) $(CFLAGS_OSXFUSE) $(CFLAGS_EXTRA) -o $@ -c $< $(LIBS)

#fs.o: fs.c fs.h fs_helper.h

#fs_helper.o: fs_helper.co fs.h fs_helper.h

clean:
	rm -f $(TARGETS) *.o
	rm -rf *.dSYM
