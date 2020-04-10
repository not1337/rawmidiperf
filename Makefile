# This file is part of the rawmidiperf project
# 
# (C) 2020 Andreas Steinmetz, ast@domdv.de
# The contents of this file is licensed under the GPL version 2 or, at
# your choice, any later version of this license.
#
# You may have to add '-latomic' e.g. on a Raspberry Pi
LIBS=-lpthread -lasound
#
OPTS=
#
# Enable the following for Raspberry Pi 4B
#
# OPTS=-march=native -mthumb -fomit-frame-pointer -fno-stack-protector

all: rawmidiperf

rawmidiperf: rawmidiperf.c
	gcc -Wall -O3 -s $(OPTS) -o rawmidiperf rawmidiperf.c $(LIBS)

clean:
	rm -f rawmidiperf
