all:	carps-decode rastertocarps
carps-decode:	carps-decode.c
	gcc -Wall -Wextra --std=c99 carps-decode.c -o carps-decode
rastertocarps:	rastertocarps.c
	gcc -Wall -Wextra --std=c99 rastertocarps.c -o rastertocarps
