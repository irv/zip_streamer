#
# Makefile for 'zip_streamer'.
#
# Type 'make' or 'make zip_streamer' to create the binary.
# Type 'make clean' or 'make clear' to delete all temporaries.
# Type 'make run' to execute the binary.
# Type 'make debug' to debug the binary using gdb(1).
#

# build target specs
CC = gcc
CFLAGS = -g -O3 -Wall -Werror -Wextra -Wpedantic -Wshadow -Wpointer-arith -Wstrict-prototypes -Wwrite-strings -Wunreachable-code -fno-omit-frame-pointer -fsanitize=address
OUT_DIR = release_build
LIBS = -lcurl -larchive -lfcgi -lmagic -llog4c -lpthread

# first target entry is the target invoked when typing 'make'
default: zip_streamer

zip_streamer: $(OUT_DIR)/zip_streamer.c.o
	@echo -n 'Linking zip_streamer... '
	@$(CC) $(CFLAGS) -o zip_streamer $(OUT_DIR)/zip_streamer.c.o $(LIBS)
	@echo Done.

$(OUT_DIR)/zip_streamer.c.o: zip_streamer.c
	@echo -n 'Compiling zip_streamer.c... '
	@$(CC) $(CFLAGS) -o $(OUT_DIR)/zip_streamer.c.o -c zip_streamer.c
	@echo Done.

run:
	spawn-fcgi -s /tmp/zip_streamer.sock -M 7777 -n ./zip_streamer 
# for degugging this likely won't be run as same user/group as web server
# hence the need to chmod the socket to 777

memcheck:
	spawn-fcgi -s /tmp/zip_streamer.sock -M 7777 -n -- /usr/bin/valgrind --leak-check=full ./zip_streamer

helgrind:
	spawn-fcgi -s /tmp/zip_streamer.sock -M 7777 -n -- /usr/bin/valgrind --tool=helgrind ./zip_streamer

debug:
	gdb -- spawn-fcgi -s /tmp/zip_streamer.sock -M 7777 -n ./zip_streamer

clean:
	@echo -n 'Removing all temporary binaries... '
	@rm -f zip_streamer $(OUT_DIR)/*.o
	@echo Done.

clear:
	@echo -n 'Removing all temporary binaries... '
	@rm -f zip_streamer $(OUT_DIR)/*.o
	@echo Done.

