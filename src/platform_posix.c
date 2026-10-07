/*
 * Platform layer for Linux and macOS: termios serial port and console.
 */
#ifndef _WIN32

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

#include "platform.h"

static int serialFd = -1;
static struct termios orgSerial;
static struct termios wrkSerial;
static int consoleModified = 0;
static struct termios orgConsole;


int serialOpen(const char * port, int speed, int rtscts)
{
	int iError;

	iError = 0;
    /* Seriellen Port fuer Ein- und Ausgabe oeffnen */
	serialFd = open(port, O_RDWR);
	if (serialFd == -1)
	{
		iError = 2;
		printf("Error: can't open device %s\r\n", port);
		printf("       (%s)\r\n", strerror(errno));
		return iError;
	}

    /* Einstellungen der seriellen Schnittstelle merken */
    if (iError == 0) /* nur wenn Port geoeffnet worden ist */
    {
        tcgetattr(serialFd, &orgSerial);
    }

    /* c_ispeed/c_ospeed are ignored by tcsetattr() on Linux,
       always use the Bxxx constants with cfsetispeed/cfsetospeed */
    switch(speed){
    case 50 :
    	speed = B50;
    	break;
    case 75 :
    	speed = B75;
    	break;
    case 110:
    	speed = B110;
    	break;
    case 134:
    	speed = B134;
    	break;
    case 150:
    	speed = B150;
    	break;
    case 200:
    	speed = B200;
    	break;
    case 300:
    	speed = B300;
    	break;
    case 600:
    	speed = B600;
    	break;
    case 1200:
    	speed = B1200;
    	break;
    case 1800:
    	speed = B1800;
    	break;
    case 2400:
    	speed = B2400;
    	break;
    case 4800:
    	speed = B4800;
    	break;
    case 9600:
    	speed = B9600;
    	break;
    case 19200:
    	speed = B19200;
    	break;
    case 38400:
    	speed = B38400;
    	break;
#ifdef B57600
    case 57600:
    	speed = B57600;
    	break;
#endif
#ifdef B115200
    case 115200:
    	speed = B115200;
    	break;
#endif
#ifdef B230400
    case 230400:
    	speed = B230400;
    	break;
#endif
#ifdef B460800
    case 460800:
    	speed = B460800;
    	break;
#endif
    default:
    	fprintf(stderr,"Baudrate not supported by this build of OpenRS.\n\rTry one of the standard Baudrates (e.g. 19200)");
    	iError = 4;
    	return iError;
    }

    /* Neue Einstellungen der seriellen Schnittstelle setzen */
    if (iError == 0)
    {
        wrkSerial = orgSerial;
        wrkSerial.c_cc[VTIME] = 0;        /* empfangene Daten     */
        wrkSerial.c_cc[VMIN] = 0;         /* sofort abliefern     */
        wrkSerial.c_iflag = IGNBRK;       /* BREAK ignorieren     */
        wrkSerial.c_oflag = 0;            /* keine Delays oder    */
        wrkSerial.c_lflag = 0;            /* Sonderbehandlungen   */
        wrkSerial.c_cflag |=  (CS8        /* 8 Bit                */
                				|CREAD      /* RX ein               */
                				|CLOCAL);   /* kein Handshake       */

        wrkSerial.c_cflag &= ~(CSTOPB     /* 1 Stop-Bit           */
                				|PARENB    	/* ohne Paritaet        */
                				|HUPCL);   	/* kein Handshake       */

#ifdef CRTSCTS
        if (rtscts)                         /* RTS/CTS Handshake    */
            wrkSerial.c_cflag |= CRTSCTS;
        else
            wrkSerial.c_cflag &= ~CRTSCTS;
#else
        if (rtscts)
        {
            iError = 4;
            printf("Error: RTS/CTS flow control not supported on this platform\r\n");
        }
#endif

        /* pty verwenden ? */
        if (speed != B0)                    /* B0 -> pty soll ver-  */
        {                                   /* wendet werden        */
        	/* Empfangsparameter setzen */
            if (cfsetispeed(&(wrkSerial), speed) == -1)
            {
                iError = 4;
                printf("Error: can't set input bitrate on %s\r\n", port);
                printf("       (%s)\r\n", strerror(errno));
            }

            /* Empfangsparameter setzen */
            if (cfsetospeed(&(wrkSerial), speed) == -1)
            {
                iError = 4;
                printf("Error: can't set output bitrate on %s\r\n", port);
                printf("       (%s)\r\n", strerror(errno));
            }
        }
    }

    /* Serielle Schnittstelle auf neue Parameter einstellen */
    if (iError == 0)
    {
        tcsetattr(serialFd, TCSADRAIN, &wrkSerial);
    }
    else
    {
		/* Fehlerbehandlung */
		/* Port war schon offen, alte Einstellungen wiederherstellen */
		if (iError > 3)
			tcsetattr(serialFd, TCSADRAIN, &orgSerial);

		/* Port war schon offen, aber noch nicht veraendert, nur schliessen */
		if (iError > 2)
		{
			close(serialFd);
		}
    }

    iError = iError != 0 ? -1 : 0;

    return iError;
}


void serialClose(void)
{
	if(serialFd != -1)
	{
		tcsetattr(serialFd, TCSADRAIN, &orgSerial);
		close(serialFd);
		serialFd = -1;
	}
}


/* the port is set to VMIN=0, VTIME=0, so read() returns at once */
int serialRead(unsigned char * buf, int len)
{
	int n = read(serialFd, buf, len);

	if(n < 0 && (errno == EAGAIN || errno == EINTR))
		return 0;
	return n;
}


int serialWrite(unsigned char b)
{
	int err;
	int errcnt = 0;

	while((err = write(serialFd, &b, 1)) != 1)
	{
		if(err == -1 && errno != EAGAIN && errno != EINTR)
		{
			perror("Unrecoverable Error while writing to serial port");
			return -1;
		}
		if(++errcnt >= 100)
		{
			fprintf(stderr, "Error writing to serial Port. Discarding some data.\r\n");
			return 1;
		}
		usleep(1000);
	}
	return 1;
}


void consoleRaw(void)
{
	struct termios t;

	if(tcgetattr(STDIN_FILENO, &orgConsole) != 0)
		return;		// not a terminal
	t = orgConsole;
	cfmakeraw(&t);
	tcsetattr(STDIN_FILENO, TCSANOW, &t);
	consoleModified = 1;
}


void consoleRestore(void)
{
	if(consoleModified)
	{
		tcsetattr(STDIN_FILENO, TCSANOW, &orgConsole);
		consoleModified = 0;
	}
}


int consoleRead(void)
{
	struct timeval tv = { 0L, 0L };
	fd_set fds;
	unsigned char c;
	int n;

	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	if(select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) <= 0)
		return CONSOLE_NO_KEY;

	n = read(STDIN_FILENO, &c, 1);
	if(n == 1)
		return c;
	if(n < 0 && (errno == EAGAIN || errno == EINTR))
		return CONSOLE_NO_KEY;
	return CONSOLE_EOF;
}


int consoleWrite(unsigned char c)
{
	return write(STDOUT_FILENO, &c, 1) == 1;
}


void sleepMs(int ms)
{
	usleep(ms * 1000);
}

#endif
