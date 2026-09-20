#!/bin/sh
# pluto_gpo_ptt.sh
#
# Shell equivalent of pluto_gpo_ptt.c, from the Analog Devices wiki:
#   https://wiki.analog.com/university/tools/pluto/hacking/power_amp
#
# Slow -- ~80 ms per swap over USB from a host, ~30-35 ms running locally on
# Pluto -- because every attribute write is a separate iio_attr process. Use
# the C version for anything that has to keep up with real traffic. This is
# handy for scoping the GPO pins and confirming the wiring.
#
# Run --restore (or the C version's --restore) before using the downconverter
# again: it needs FDD, and this leaves the part in TDD.

set -e

DEV=ad9361-phy
GPO_RX=0
GPO_TX=1

setup() {
    # 0 = TDD, 1 = FDD. GPO slaving only applies in TDD.
    iio_attr -q -a -D $DEV adi,frequency-division-duplex-mode-enable 0
    iio_attr -q -a -D $DEV adi,gpo${GPO_RX}-slave-rx-enable 1
    iio_attr -q -a -D $DEV adi,gpo${GPO_TX}-slave-tx-enable 1
    iio_attr -q -a -D $DEV initialize 1
    # Should list "rx tx" and not "fdd".
    iio_attr -a -d $DEV ensm_mode_available
}

restore() {
    iio_attr -q -a -D $DEV adi,frequency-division-duplex-mode-enable 1
    iio_attr -q -a -D $DEV initialize 1
    iio_attr -a -d $DEV ensm_mode_available
}

case "$1" in
    --setup)
        setup
        iio_attr -q -a -d $DEV ensm_mode rx
        ;;
    --restore)
        restore
        ;;
    --state)
        [ -n "$2" ] || { echo "usage: $0 --state rx|tx|alert|wait|sleep" >&2; exit 1; }
        iio_attr -q -a -d $DEV ensm_mode "$2"
        ;;
    --bench)
        setup
        trap 'iio_attr -q -a -d $DEV ensm_mode rx; exit 0' INT TERM
        while true ; do
            iio_attr -q -a -d $DEV ensm_mode rx
            # capture buffer here
            iio_attr -q -a -d $DEV ensm_mode tx
            # transmit buffer here
        done
        ;;
    *)
        echo "usage: $0 --setup | --state MODE | --bench | --restore" >&2
        exit 1
        ;;
esac
