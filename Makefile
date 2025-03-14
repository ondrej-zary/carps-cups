CFLAGS=-Wall -Wextra --std=c99 -O2
CUPSDIR=$(shell cups-config --serverbin)
CUPSDATADIR=$(shell cups-config --datadir)

all:	carps-decode rastertocarps ppd/*.ppd

carps-decode:	carps-decode.c carps.h
	gcc $(CFLAGS) carps-decode.c -o carps-decode

rastertocarps:	rastertocarps.c carps.h
	gcc $(CFLAGS) rastertocarps.c -o rastertocarps -lcupsimage -lcups -ltiff

ppd/*.ppd: carps.drv
	ppdc carps.drv

clean:
	rm -f carps-decode rastertocarps

install: rastertocarps
	install -s rastertocarps $(CUPSDIR)/filter/
	install -m 644 carps.drv $(CUPSDATADIR)/drv/
	install -m 644 carps.usb-quirks $(CUPSDATADIR)/usb/
