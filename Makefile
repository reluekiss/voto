CC      := gcc
CFLAGS  := -ggdb -Os -Wall -Wextra \
           -ffunction-sections -fdata-sections
LDFLAGS := -static -lopus -lpthread -lssl -lcrypto -ldl -lm

SRC     := main.c miniaudio.c
OBJ     := $(SRC:.c=.o)
TARGET  := voto

.PHONY: all ssl clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

ssl:
	openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/"

clean:
	rm -f *.pem $(OBJ) $(TARGET)
