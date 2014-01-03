all:	carps-decode rastertocarps ppds
carps-decode:	carps-decode.c carps.h
	gcc -Wall -Wextra --std=c99 carps-decode.c -o carps-decode
rastertocarps:	rastertocarps.c carps.h
	gcc -Wall -Wextra --std=c99 rastertocarps.c -lcupsimage -o rastertocarps
ppds: carps.drv
	ppdc carps.drv
