# OpenRS
File transfer and terminal for TNC3 / TNC4

For those still owning and using a TNC3/TNC4. (Another DL1GJI software and protocol reverse-engineered... :) )

It may still need some polishing, but flashing and transferring files from and to the ramdisk should work.
Runs on Linux, macOS and Windows.

### Usage

    openrs [-r] [-c seconds] <serialPort> [speed [tnc command]]

`serialPort` is e.g. `/dev/ttyUSB0` (Linux), `/dev/tty.usbserial-…` (macOS) or `COM3` (Windows).
`-r` enables RTS/CTS hardware flow control (only with a cable that carries the handshake lines). A TNC command after the speed is typed on the TNC once the port is open; `-c`/`--close-after` ends OpenRS after the given number of seconds, e.g. `openrs -c 60 /dev/ttyUSB0 19200 'cp c:\prog.apl r:prog.apl'`. On the TNC, files on the PC are addressed with the drive letter `c:`, e.g. `cp c:\prog.apl r:prog.apl` or `ls c:\*.*`. OpenRS serves files from the current directory only.

### Build and test

    make
    make test
    make install        # to /usr/local/bin, or PREFIX=...

On Windows, use MinGW (`mingw32-make`). Without make: `cc -O2 -Wall -o openrs src/*.c`. The tests play the TNC side of the protocol over a pseudo terminal, so no TNC is needed; they run on Linux and macOS, and against a Windows build under wine (`make test EXE=.exe RUNNER=wine`). The Makefile header shows how to cross-compile.

----

Dateitransfer und Terminal für TNC3 / TNC4

Ersatz für die Programme rs.exe/rs32.exe zum flashen des TNC3/TNC4 Speichers und zum Datentransfer in/aus der Ramdisk.

