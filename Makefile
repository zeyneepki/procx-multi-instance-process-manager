CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread
LDFLAGS=-lrt

TARGET=procx
SRC=procx.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)