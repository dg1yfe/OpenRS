/*
 * Platform layer for Windows: Win32 serial port and console.
 */
#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static HANDLE serial = INVALID_HANDLE_VALUE;
static DCB orgDcb;
static COMMTIMEOUTS orgTimeouts;

static HANDLE conIn = INVALID_HANDLE_VALUE;
static HANDLE conOut = INVALID_HANDLE_VALUE;
static DWORD orgInMode, orgOutMode;
static int inIsConsole = 0, outIsConsole = 0;

// redirected input (pipe or file) is read by a thread into this ring buffer,
// since not every kind of handle can be polled
static CRITICAL_SECTION inLock;
static HANDLE inThread = NULL;
static unsigned char inBuf[256];
static int inHead = 0, inTail = 0, inEof = 0;


static void printLastError(const char * what, const char * port)
{
	char msg[256];

	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
			GetLastError(), 0, msg, sizeof msg, NULL);
	printf("Error: %s %s\r\n       (%s)\r\n", what, port, msg);
}


/* "COM3" or "\\.\COM12"; COM10 and above need the \\.\ prefix */
int serialOpen(const char * port, int bitrate, int rtscts)
{
	char name[300];
	DCB dcb;
	COMMTIMEOUTS to;

	if(strncmp(port, "\\\\", 2) == 0)
		snprintf(name, sizeof name, "%s", port);
	else
		snprintf(name, sizeof name, "\\\\.\\%s", port);

	serial = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if(serial == INVALID_HANDLE_VALUE)
	{
		printLastError("can't open device", port);
		return -1;
	}

	memset(&orgDcb, 0, sizeof orgDcb);
	orgDcb.DCBlength = sizeof orgDcb;
	if(!GetCommState(serial, &orgDcb) || !GetCommTimeouts(serial, &orgTimeouts))
	{
		printLastError("can't read settings of", port);
		CloseHandle(serial);
		serial = INVALID_HANDLE_VALUE;
		return -1;
	}

	dcb = orgDcb;
	dcb.BaudRate = bitrate;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fParity = FALSE;
	dcb.fOutxCtsFlow = rtscts ? TRUE : FALSE;
	dcb.fRtsControl = rtscts ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fDsrSensitivity = FALSE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fErrorChar = FALSE;
	dcb.fNull = FALSE;
	dcb.fAbortOnError = FALSE;
	if(!SetCommState(serial, &dcb))
	{
		printLastError("can't set bitrate or format on", port);
		CloseHandle(serial);
		serial = INVALID_HANDLE_VALUE;
		return -1;
	}

	// reads return at once with what is there, writes block until sent
	memset(&to, 0, sizeof to);
	to.ReadIntervalTimeout = MAXDWORD;
	SetCommTimeouts(serial, &to);
	return 0;
}


void serialClose(void)
{
	if(serial != INVALID_HANDLE_VALUE)
	{
		SetCommState(serial, &orgDcb);
		SetCommTimeouts(serial, &orgTimeouts);
		CloseHandle(serial);
		serial = INVALID_HANDLE_VALUE;
	}
}


int serialRead(unsigned char * buf, int len)
{
	DWORD n;

	if(!ReadFile(serial, buf, len, &n, NULL))
	{
		DWORD errors;

		// a line error (framing, overrun) is reported once; clear it and go on
		if(ClearCommError(serial, &errors, NULL))
			return 0;
		return -1;
	}
	return (int) n;
}


int serialWrite(unsigned char b)
{
	DWORD n;

	if(!WriteFile(serial, &b, 1, &n, NULL) || n != 1)
	{
		DWORD errors;

		if(ClearCommError(serial, &errors, NULL) && WriteFile(serial, &b, 1, &n, NULL) && n == 1)
			return 1;
		fprintf(stderr, "Unrecoverable Error while writing to serial port.\r\n");
		return -1;
	}
	return 1;
}


static DWORD WINAPI inputThread(LPVOID arg)
{
	unsigned char c;
	DWORD n;

	(void) arg;
	for(;;)
	{
		if(!ReadFile(conIn, &c, 1, &n, NULL) || n != 1)
		{
			EnterCriticalSection(&inLock);
			inEof = 1;
			LeaveCriticalSection(&inLock);
			return 0;
		}
		for(;;)
		{
			int next;

			EnterCriticalSection(&inLock);
			next = (inHead + 1) % (int) sizeof inBuf;
			if(next != inTail)
			{
				inBuf[inHead] = c;
				inHead = next;
				LeaveCriticalSection(&inLock);
				break;
			}
			LeaveCriticalSection(&inLock);
			Sleep(1);		// buffer full, wait for the main loop
		}
	}
}


void consoleRaw(void)
{
	conIn = GetStdHandle(STD_INPUT_HANDLE);
	conOut = GetStdHandle(STD_OUTPUT_HANDLE);

	if(GetConsoleMode(conIn, &orgInMode))
	{
		inIsConsole = 1;
		// no line editing, no echo, CTRL-C arrives as a key (0x03)
		SetConsoleMode(conIn, orgInMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
	}
	else if(!inThread)
	{
		InitializeCriticalSection(&inLock);
		inThread = CreateThread(NULL, 0, inputThread, NULL, 0, NULL);
		if(!inThread)
			inEof = 1;
	}
	if(GetConsoleMode(conOut, &orgOutMode))
	{
		outIsConsole = 1;
		// let the console interpret the TNC's escape sequences (Windows 10 and later)
		SetConsoleMode(conOut, orgOutMode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
}


void consoleRestore(void)
{
	if(inIsConsole)
		SetConsoleMode(conIn, orgInMode);
	if(outIsConsole)
		SetConsoleMode(conOut, orgOutMode);
	inIsConsole = outIsConsole = 0;
}


int consoleRead(void)
{
	static int repeat = 0;
	static unsigned char key;

	if(repeat > 0)
	{
		repeat--;
		return key;
	}

	if(inIsConsole)
	{
		INPUT_RECORD rec;
		DWORD n;

		while(GetNumberOfConsoleInputEvents(conIn, &n) && n > 0)
		{
			if(!ReadConsoleInputA(conIn, &rec, 1, &n) || n != 1)
				return CONSOLE_EOF;
			// only key presses that produce a character (no shift, arrows, ...)
			if(rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown
					&& rec.Event.KeyEvent.uChar.AsciiChar)
			{
				key = (unsigned char) rec.Event.KeyEvent.uChar.AsciiChar;
				repeat = rec.Event.KeyEvent.wRepeatCount - 1;
				return key;
			}
		}
		return CONSOLE_NO_KEY;
	}
	else if(inThread)
	{
		int c = CONSOLE_NO_KEY;

		EnterCriticalSection(&inLock);
		if(inHead != inTail)
		{
			c = inBuf[inTail];
			inTail = (inTail + 1) % (int) sizeof inBuf;
		}
		else if(inEof)
		{
			c = CONSOLE_EOF;
		}
		LeaveCriticalSection(&inLock);
		return c;
	}
	return CONSOLE_EOF;
}


int consoleWrite(unsigned char c)
{
	DWORD n;

	if(conOut == INVALID_HANDLE_VALUE)
		conOut = GetStdHandle(STD_OUTPUT_HANDLE);
	return WriteFile(conOut, &c, 1, &n, NULL) && n == 1;
}


void sleepMs(int ms)
{
	Sleep(ms);
}


unsigned long long timeMs(void)
{
	return GetTickCount64();
}

#endif
