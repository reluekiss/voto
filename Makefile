CC      := clang
CFLAGS  := -ggdb -Os -Wall -Wextra \
           -ffunction-sections -fdata-sections
LDFLAGS := -lopus -lpthread -lssl -lcrypto -ldl -lm -Wl,--gc-sections

SRC_C   := main.c nat_traversal.c
SRC_H   := miniaudio.h coroutine.h
OBJ     := main.o miniaudio.o coroutine.o nat_traversal.o

TARGET  := voto

.PHONY: all debug ssl clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -flto $(CFLAGS) -o $@ $^ $(LDFLAGS)
	strip $(TARGET)

debug: CFLAGS += -DDEBUG
debug: $(TARGET)

main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

nat_traversal.o: nat_traversal.c
	$(CC) $(CFLAGS) -c $< -o $@

miniaudio.o: miniaudio.h
	$(CC) -x c -DMINIAUDIO_IMPLEMENTATION \
	      $(CFLAGS) -c $< -o $@

coroutine.o: coroutine.h
	$(CC) -x c -DCOROUTINE_IMPLEMENTATION \
	      $(CFLAGS) -c $< -o $@

ssl:
	openssl req -x509 -newkey rsa:4096 \
		-keyout key.pem -out cert.pem \
		-days 365 -nodes -subj "/"

clean:
	rm -f *.pem $(OBJ) $(TARGET)
