CC = gcc
CFLAGS = -Wall -Wextra

all: minls minget

# Target 1: minls executable
minls: minls.o fs_util.o
	$(CC) $(CFLAGS) minls.o fs_util.o -o minls

# Target 2: minget executable
minget: minget.o fs_util.o
	$(CC) $(CFLAGS) minget.o fs_util.o -o minget

# Rule for building object files from C sources
%.o: %.c fs_util.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f minls minget *.o