CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11
LDFLAGS ?=
TARGET = aesdsocket
SRC = aesdsocket.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)
default: all

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: all default clean