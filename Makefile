CC ?= gcc
CFLAGS += -Wall -Wextra --std=c99
CUPSDIR = $(DESTDIR)$(shell cups-config --serverbin)
CUPSDATADIR = $(DESTDIR)$(shell cups-config --datadir)

all:	carps-decode rastertocarps ppd/*.ppd

carps-decode:	carps-decode.c carps.h
	$(CC) $(CFLAGS) carps-decode.c -o carps-decode $(LDFLAGS)

rastertocarps:	rastertocarps.c carps.h
	$(CC) $(CFLAGS) rastertocarps.c -o rastertocarps -lcupsimage -lcups $(LDFLAGS)

ppd/*.ppd: carps.drv
	ppdc carps.drv

clean:
	rm -f carps-decode rastertocarps

install: rastertocarps
	mkdir -p $(CUPSDIR)/filter/
	mkdir -p $(CUPSDATADIR)/drv/
	mkdir -p $(CUPSDATADIR)/usb/
	install -m 755 rastertocarps $(CUPSDIR)/filter/
	install -m 644 carps.drv $(CUPSDATADIR)/drv/
	install -m 644 carps.usb-quirks $(CUPSDATADIR)/usb/
