#!/bin/sh

set -ux

openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/"

gcc -Os -Wall -Wextra -o voicechat main.c -lopus -lssl -lcrypto -pthread -lm -ffunction-sections -fdata-sections
