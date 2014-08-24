CUPS driver for Canon CARPS printers
====================================

This provides rastertocups filter and PPD files (specified by carps.drv file) which
allows these printers to print from Linux and possibly any other OS where CUPS is used.

carps-decode is a debug tool - it decodes CARPS data (created either by rastertocups
filter or windows drivers), producing a PBM bitmap and debug output.

Printers known to use CARPS data format:

Printer type (IEEE1284 ID)	| Status
--------------------------------|--------------------------------------------------------
MF5730				| works
MF5750				| should work
MF5770				| should work
MF5630				| should work
MF5650				| should work
MF3110				| works
imageCLASS D300			| works
LASERCLASS 500			| should work
FP-L170/MF350/L380/L398		| should work
LC310/L390/L408S		| works
PC-D300/FAX-L400/ICD300		| works
L180/L380S/L398S		| should work
L120				| not supported - different data format
MF3200 Series			| not supported - different data format, different header
MF8100 Series			| not supported - different data format, color

Compiling from source
---------------------
Requirements: make, gcc, libcups2-dev, libcupsimage2-dev, cups-ppdc

To compile, simply run "make":

    $ make

To install compiled filter and drv file, run "make install" as root:

    # make install

or

    $ sudo make install

You can then install the printer using standard GUI tools or CUPS web interface.


Problems with CUPS libusb backend
---------------------------------
The libusb backend used by CUPS since 1.4.x is crap. The code is full of quirks for
various printers and it's no surprise that it does not work properly with CARPS printers
(at least MF5730 and D320) too - the first document prints but nothing more is printed until the
printer is turned off and on again.

The solution is to set printer URI to the usblp device, e.g. "file:///dev/usb/lp0".
For this to work, file: device URIs must be enabled in CUPS configuration:
(/etc/cups/cups-files.conf)

    FileDevice Yes


Multiple USB printers
---------------------

If you have multiple USB printers, the usblp devices might be assigned differently on each boot or hot-plug. To avoid this, you can create an udev rule like this (example from Ubuntu 14.04 and Canon D320):

    SUBSYSTEMS=="usb", ATTRS{ieee1284_id}=="MFG:Canon;MDL:imageCLASS D300;CLS:PRINTER;DES:Canon imageCLASS D300;CID:;CMD:LIPS;", SYMLINK+="canonD320"


You can find your printer's IEEE1284 ID by running:

    udevadm info -a --name=/dev/usb/lpX

When you are done restart udev:
    sudo service udev restart

Ensure that /dev/canonD320 points to /dev/usb/lpX

Now you can map your printer as file:///dev/canonD320
