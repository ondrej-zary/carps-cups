CUPS driver for Canon CARPS printers
====================================

Printers known to use CARPS data format:

Printer type (IEEE1284 ID)	| Status
--------------------------------|--------------------------------------------------------
MF5730				| works
MF5750				| should work
MF5770				| should work
MF5630				| should work
MF5650				| should work
MF3110				| should work
imageCLASS D300			| should work
LASERCLASS 500			| should work
FP-L170/MF350/L380/L398		| should work
LC310/L390/L408S		| should work
PC-D300/FAX-L400/ICD300		| should work
L180/L380S/L398S		| should work
L120				| not supported - different data format
MF3200 Series			| not supported - different data format, different header
MF8100 Series			| not supported - different data format, color


Problems with CUPS libusb backend
---------------------------------
The libusb backend used by CUPS since 1.4.x is crap. The code is full of quirks for
various printers and it's no surprise that it does not work properly with CARPS printers
(at least MF5730) too - the first document prints but nothing more is printed until the
printer is turned off and on again.

The solution is to set printer URI to the usblp device, e.g. "file:///dev/usb/lp0".
For this to work, file: device URIs must be enabled in CUPS configuration:
(/etc/cups/cups-files.conf)

    FileDevice Yes
