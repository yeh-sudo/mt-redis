# Variables
CC = gcc
CFLAGS = -std=gnu11
TARGET = neco
OBJECTS = $(TARGET).o

# Default target
all: $(OBJECTS)

# Compile .c to .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up the build
clean:
	rm -f $(OBJECTS)