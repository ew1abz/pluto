# Cross-build the downconverter and its DSP test for Pluto.
#
#   make                       # uses the plutosdr-fw staging tree
#   make STAGING=/path/to/staging CROSS_COMPILE=arm-linux-gnueabihf-
#   make dsp_test              # numerical checks, run it on the target
#
# The DSP test exercises the NEON path, so run it on Pluto rather than a host.

FW      ?= ../../firmware/plutosdr-fw/buildroot/output
STAGING ?= $(FW)/staging
CROSS_COMPILE ?= $(FW)/host/bin/arm-none-linux-gnueabihf-

CC      := $(CROSS_COMPILE)gcc
STRIP   := $(CROSS_COMPILE)strip
CFLAGS  := -O2 -Wall -mfpu=neon -mfloat-abi=hard -I$(STAGING)/usr/include
LDFLAGS := -L$(STAGING)/usr/lib
LDLIBS  := -liio -lad9361 -lm

BINDIR  := ../bin

all: pluto_downconverter

pluto_downconverter: pluto_downconverter.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

dsp_test: dsp_test.c pluto_downconverter.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# stripped copy in apps/bin for scp'ing to the target
install: pluto_downconverter
	$(STRIP) -o $(BINDIR)/pluto_downconverter pluto_downconverter

clean:
	rm -f pluto_downconverter dsp_test

.PHONY: all install clean
