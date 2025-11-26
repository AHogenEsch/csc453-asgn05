CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99 

# Object files for the shared library
MINIX_FS_OBJS = fs_util.o

# Target 1: minls executable
minls: minls.o $(MINIX_FS_OBJS)
	$(CC) $(CFLAGS) minls.o $(MINIX_FS_OBJS) -o minls

# Target 2: minget executable
minget: minget.o $(MINIX_FS_OBJS)
	$(CC) $(CFLAGS) minget.o $(MINIX_FS_OBJS) -o minget

all: minls minget

# Rule for building object files from C sources
%.o: %.c fs_util.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f minls minget *.o