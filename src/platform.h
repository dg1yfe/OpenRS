/*
 * Platform layer: serial port, console and timing.
 * platform_posix.c implements it for Linux and macOS, platform_win32.c for
 * Windows. Each compiles to nothing on the other platforms, so all sources
 * in src/ can always be built together.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

/* open and configure the serial port (8N1, raw); 0 on success */
int serialOpen(const char * port, int bitrate, int rtscts);
void serialClose(void);
/* read what is available without blocking; bytes read, 0 if none, -1 on error */
int serialRead(unsigned char * buf, int len);
/* write one byte; 1 on success, -1 on an unrecoverable error */
int serialWrite(unsigned char b);

/* switch the console to raw mode (no echo, no line editing, CTRL-C as 0x03) */
void consoleRaw(void);
void consoleRestore(void);
/* next key without blocking: 0..255, CONSOLE_NO_KEY or CONSOLE_EOF */
#define CONSOLE_NO_KEY	(-1)
#define CONSOLE_EOF		(-2)
int consoleRead(void);
/* write one character from the TNC to the console; 1 on success */
int consoleWrite(unsigned char c);

void sleepMs(int ms);
/* milliseconds from a monotonic clock */
unsigned long long timeMs(void);

#endif
