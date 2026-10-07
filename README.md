# OpenRS
File transfer and terminal for TNC3 / TNC4

For those still owning and using a TNC3/TNC4. (Another DL1GJI software and protocol reverse-engineered... :) )

It may still need some polishing, but flashing and transferring files from and to the ramdisk should work.
Tested on OS-X and Linux...

### Usage

    openrs [-r] <serialPort> [speed]

`-r` enables RTS/CTS hardware flow control (only with a cable that carries the handshake lines). On the TNC, files on the PC are addressed with the drive letter `c:`, e.g. `cp c:\prog.apl r:prog.apl` or `ls c:\*.*`. OpenRS serves files from the current directory only.

### Build and test

    cc -O2 -Wall -o openrs src/OpenRS.c
    python3 tests/protocol_test.py ./openrs

The tests play the TNC side of the protocol over a pseudo terminal; no TNC is needed.

### Releases

Pushing an annotated tag `v<version>` builds static Linux binaries (x86-64, aarch64, armv7, armv6) and a universal macOS binary, tests them and attaches them to a draft release. The tag annotation becomes the release notes.

----

Dateitransfer und Terminal für TNC3 / TNC4

Ersatz für die Programme rs.exe/rs32.exe zum flashen des TNC3/TNC4 Speichers und zum Datentransfer in/aus der Ramdisk.

