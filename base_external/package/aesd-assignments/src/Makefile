CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?= -lpthread -lrt
TARGET ?= aesdsocket

SRCS = aesdsocket.c

all: $(TARGET)

$(TARGET): $(SRCS)
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@chmod +x $@

clean:
	@rm -f $(TARGET)

.PHONY: all clean
