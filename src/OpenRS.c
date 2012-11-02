/*
 ============================================================================
 Name        : OpenRS.c
 Author      : F. Erckenbrecht / dg1yfe
 Version     : 1.0
 Copyright   : GPL
 Description : Terminal for TNC3/TNC4e with support for file transfer
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <errno.h>

#define DEFAULT_BITRATE 19200;

struct termios org_termios;
struct termios wrk_termios;
struct termios org_termios_console;
struct termios wrk_termios_console;

int iDescriptor=-1;
FILE * iFileDescriptor=NULL;
int iConsoleSettingsModified = 0;

int openSerial(char * port, int speed);

void restoreState(void)
{
    fprintf(stdout,"\nExiting...\n");
    if(iDescriptor != -1)
    {
    	tcsetattr(iDescriptor, TCSADRAIN, &org_termios);
    	close(iDescriptor);
    	iDescriptor = -1;
    }

    if(iConsoleSettingsModified)
    	tcsetattr(0, TCSANOW, &org_termios_console);

	if(iFileDescriptor)
    	fclose(iFileDescriptor);
}


void restoreStateSig(int sig)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

	restoreState();

	exit(0);
}


int kbhit()
{
    struct timeval tv = { 0L, 0L };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return select(1, &fds, NULL, NULL, &tv);
}


int getch()
{
    int r;
    unsigned char c;

    if ((r = read(0, &c, sizeof(c))) < 0)
    {
        return r;
    }
    else
    {
        return c;
    }
}


int main(int argc, char *argv[]) {
/*
 *
 * parse command line options - libpopt?
 * open serial port
 * loop
 * 	read keyboard
 * 	transfer byte
 * 	read serial
 * 	perform protocol handling
 * 		eventually write to console
 * 		eventually access files
 *
 */

	char port[PATH_MAX];
	char filename[PATH_MAX];
	char * command = NULL;
	int bitrate = DEFAULT_BITRATE;
	int i;

	if(argc > 1)
	{
		if(strlen(argv[1]) >= PATH_MAX)
		{
			fprintf(stderr, "Invalid device name. Name exceeds %d bytes (PATH_MAX)" ,PATH_MAX);
			exit(1);
		}
		strncpy(port,argv[1],PATH_MAX);

		if(argc>2)
		{
			int c;

			c = sscanf(argv[2], "%d", &bitrate);
			if(c==0)
			{
				bitrate = DEFAULT_BITRATE;
				fprintf(stderr, "Could not parse bitrate. Argument 2 ignored.\n");
				fprintf(stderr, "Bitrate defaults to %d bps.\n", bitrate);
			}
		}

		if(argc>3)
		{
			int i;
			int len = 0;
			char * cmd;

			for(i=3;i<argc;i++)
			{
				len += strlen(argv[i]);
			}
			len+=argc-3;	// include spaces;
			len+=1;			// include terminating zero

			command = malloc(len);
			if(command==NULL)
			{
				perror("Sorry, could not allocate memory for commands.\nExiting...\n");
				exit(1);
			}

			memset(command,0,len);
			cmd = command;
			for(i=3;i<argc;i++)
			{
				if(len<=0)
					break;
				strncpy(cmd, argv[i], len);
				len -= strlen(argv[i]);
				cmd += strlen(argv[i]);
				if(len<=0)
					break;
				*cmd++=' ';
				len--;
			}
			*cmd=0;
		}
	}
	else
	{
		perror("Please specify serial device and (optionally) speed (default: 19200).\n");
		perror("Exit with CTRL-C\n\n");
		perror("Usage: openrs <serialPort> <speed> <tnc command>\n");
		perror("Example:\nopenrs /dev/tty.usb 19200 flash epflash.bin\n\n");
		exit(1);
	}

	tcgetattr(0, &org_termios_console);
	wrk_termios_console = org_termios_console;

    signal(SIGINT,restoreStateSig);
    signal(SIGTERM,restoreStateSig);
    atexit(restoreState);

    if(openSerial(port, bitrate)!=0)
    {
    	exit(1);
    }

    iConsoleSettingsModified=1;
    cfmakeraw(&wrk_termios_console);
    tcsetattr(0, TCSANOW, &wrk_termios_console);


    while(1)
    {
    	if(kbhit())
    	{
    		int ch;
    		ch=getch();
    		write(0,&ch,1);
    	}
    	else
    	{

    	}
    }

	return EXIT_SUCCESS;
}


int openSerial(char * port, int speed)
{
	int iError;
#ifndef __APPLE__
    struct  serial_struct ser_io;
#endif

	iError = 0;
    /* Seriellen Port fuer Ein- und Ausgabe oeffnen */
	iDescriptor = open(port, O_RDWR);
	if (iDescriptor == -1)
	{
		iError = 2;
		printf("Error: can't open device %s\n", port);
		printf("       (%s)\n", strerror(errno));
	}

    /* Einstellungen der seriellen Schnittstelle merken */
    if (iError == 0) /* nur wenn Port geoeffnet worden ist */
    {
        tcgetattr(iDescriptor, &org_termios);
#ifndef __APPLE__
        if (speed == B38400)                        /* >= 38400 Bd  */
        {
            if (ioctl(iDescriptor, TIOCGSERIAL, &ser_io) < 0)
            {
                iError = 3;
                printf("Error: can't get actual settings for device %s\n", port);
                printf("       (%s)\n", strerror(errno));
            }
        }
#endif
    }

    /* Neue Einstellungen der seriellen Schnittstelle setzen */
    if (iError == 0)
    {
        wrk_termios = org_termios;
        wrk_termios.c_cc[VTIME] = 0;        /* empfangene Daten     */
        wrk_termios.c_cc[VMIN] = 0;         /* sofort abliefern     */
        wrk_termios.c_iflag = IGNBRK;       /* BREAK ignorieren     */
        wrk_termios.c_oflag = 0;            /* keine Delays oder    */
        wrk_termios.c_lflag = 0;            /* Sonderbehandlungen   */
        wrk_termios.c_cflag |=  (CS8        /* 8 Bit                */
                				|CREAD      /* RX ein               */
                				|CLOCAL);   /* kein Handshake       */

        wrk_termios.c_cflag &= ~(CSTOPB     /* 1 Stop-Bit           */
                				|PARENB    	/* ohne Paritaet        */
                				|HUPCL);   	/* kein Handshake       */

        /* pty verwenden ? */
        if (speed != B0)                    /* B0 -> pty soll ver-  */
        {                                         /* wendet werden        */
            /* Empfangsparameter setzen */
            if (cfsetispeed(&(wrk_termios), speed) == -1)
            {
                iError = 4;
                printf("Error: can't set input bitrate on %s\n", port);
                printf("       (%s)\n", strerror(errno));
            }

            /* Empfangsparameter setzen */
            if (cfsetospeed(&(wrk_termios), speed) == -1)
            {
                iError = 4;
                printf("Error: can't set output bitrate on %s\n", port);
                printf("       (%s)\n", strerror(errno));
            }
#ifndef __APPLE__
            if (speed == B38400)              /* wenn >= 38400 Bd     */
            {
                ser_io.flags &= ~ASYNC_SPD_MASK;      /* Speed-Flag -> 0      */
                ser_io.flags |= speedflag;      /* Speed-Flag setzen    */

                if (ioctl(NewRing->iDescriptor, TIOCSSERIAL, &ser_io) < 0)
                {
                    iError = 4;
                    printf("Error: can't set device settings on port %s\n", port);
                    printf("       (%s)\n", strerror(errno));
                }
            }
#endif
        }
    }

    /* Serielle Schnittstelle auf neue Parameter einstellen */
    if (iError == 0)
    {
        tcsetattr(iDescriptor, TCSADRAIN, &wrk_termios);
    }
    else
    {
		/* Fehlerbehandlung */
		/* Port war schon offen, alte Einstellungen wiederherstellen */
		if (iError > 3)
			tcsetattr(iDescriptor, TCSADRAIN, &org_termios);

		/* Port war schon offen, aber noch nicht veraendert, nur schliessen */
		if (iError > 2)
		{
			close(iDescriptor);
		}
    }

    iError = iError != 0 ? -1 : 0;

    return iError;
}
