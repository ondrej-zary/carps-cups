#!/bin/sh

test_decode() {
	echo -n "$1: "
	./carps-decode $1.prn >$1.out
	mv decoded-p1.pbm $1.pbm.decodetest
	cmp $1.pbm $1.pbm.decodetest
	if [ "$?" = "0" ]; then
		echo OK
	fi
}

test_decode oneline
test_decode web1
test_decode testpage
test_decode sunset-dither
test_decode waterlilies-dither
test_decode bluehills-dither
test_decode screenshot
test_decode random
test_decode waterlilies
test_decode bluehills
test_decode sunset
