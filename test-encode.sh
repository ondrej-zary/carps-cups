#!/bin/sh

test_encode() {
	echo -n "$1: "
	./rastertocarps $1.pbm- >$1.test 2>$1.out
	./carps-decode $1.test >/dev/null
	cmp $1.pbm decoded-p1.pbm
	if [ "$?" = "0" ]; then
		echo OK
	fi
}

test_encode oneline
test_encode web1
test_encode testpage
test_encode sunset-dither
test_encode waterlilies-dither
test_encode bluehills-dither
test_encode screenshot
test_encode waterlilies
test_encode bluehills
test_encode sunset
