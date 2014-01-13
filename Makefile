CFLAGS=-Wall -Wextra --std=c99 -O2

all:	carps-decode rastertocarps ppd/*.ppd

carps-decode:	carps-decode.c carps.h
	gcc $(CFLAGS) carps-decode.c -o carps-decode

rastertocarps:	rastertocarps.c carps.h
	gcc $(CFLAGS) rastertocarps.c -lcupsimage -o rastertocarps

ppd/*.ppd: carps.drv
	ppdc carps.drv
