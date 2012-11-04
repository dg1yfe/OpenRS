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
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#ifdef __APPLE__
	#define htobe16(x)      ((u_int16_t)htons((u_int16_t)(x)))
	#define htobe32(x)      ((u_int32_t)htonl((u_int32_t)(x)))
#else
	#include <endian.h>
#endif

#define DEFAULT_BITRATE 19200;

enum {CMD_FOPEN, CMD_FREAD, CMD_FWRITE, CMD_FCLOSE,
	CMD_FGETC, CMD_FPUTC, CMD_FGETS, CMD_FPUTS,
	CMD_FINDFIRST, CMD_FINDNEXT,
	CMD_REMOVE, CMD_RENAME,
	CMD_FTELL, CMD_FSEEK,
	CMD_UNGETC
};

enum { STATE_IDLE, STATE_GETCMD, STATE_PROCESS};
enum { GET_IDLE, GET_STRING1, GET_STRING2, GET_DW, GET_W, GET_FD };


typedef struct ff_fdate{
	unsigned 		day:5;
	unsigned		month:4;
	unsigned 		year:7;	// Jahre seit 1980
}t_ffdate;

typedef struct ff_ftime{
	unsigned		sek_2:5;	// Zählung in Schritten von 2 Sekunden
	unsigned		min:6;
	unsigned		hour:5;
}t_fftime;

struct FileInfo{
	uint16_t	attr;
	t_ffdate	LastWriteDate;
	t_fftime	LastWriteTime;
	uint32_t	filesize;
	char		filename[14]; 	// sprintf(&FileInfo.filename,"%-1.13s", Dateiname))
};

struct termios org_termios;
struct termios wrk_termios;
struct termios org_termios_console;
struct termios wrk_termios_console;

int iDescriptor=-1;
int iConsoleSettingsModified = 0;

#define MAXFPTR 256
FILE * File[MAXFPTR];	// since TNC3OS soes not support 64 Bit pointers, but
					// wants to handle "File *" by itself, we do a mapping
					// using a table. Instead of File * we return a table
					// index
int fptr;

void protocolHandler(char c);
int openSerial(char * port, int speed);

void restoreState(void)
{
	int i;

    fprintf(stdout,"\n\rExiting...\n\r");
    if(iDescriptor != -1)
    {
    	tcsetattr(iDescriptor, TCSADRAIN, &org_termios);
    	close(iDescriptor);
    	iDescriptor = -1;
    }

    if(iConsoleSettingsModified)
    	tcsetattr(0, TCSANOW, &org_termios_console);

	for(i=1;i<=MAXFPTR;i++)
	{
		if(File[i-1])
			fclose(File[i-1]);
    	File[i-1] = NULL;
	}
}


void restoreStateSig(int sig)
{

//	restoreState();

	exit(0);
}


int dataAvailable(int iDescriptor)
{
    struct timeval tv = { 0L, 0L };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(iDescriptor, &fds);
    return select(iDescriptor+1, &fds, NULL, NULL, &tv);
}


int getch()
{
    int r;
    int c;

    if ((r = read(0, &c, sizeof(c))) < 0)
    {
        return r;
    }
    else
    {
        return c & 0xff;
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
	char * command = NULL;
	int bitrate = DEFAULT_BITRATE;
	char data[1024];
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
				printf("Sorry, could not allocate memory for commands.\nExiting...\n");
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
		printf("\nPlease specify serial device and (optionally) speed (default: 19200).\n");
		printf("Usage: openrs <serialPort> <speed> <tnc command>\n");
		printf("Exit with CTRL-C\n\n");
		printf("Example:\nopenrs /dev/tty.usb 19200 flash epflash.bin\n\n");
		exit(0);
	}

	tcgetattr(0, &org_termios_console);
	wrk_termios_console = org_termios_console;

    atexit(restoreState);
    signal(SIGINT,restoreStateSig);
    signal(SIGTERM,restoreStateSig);

    if(openSerial(port, bitrate)!=0)
    {
    	exit(1);
    }

    iConsoleSettingsModified=1;
    cfmakeraw(&wrk_termios_console);
    tcsetattr(0, TCSANOW, &wrk_termios_console);

    fptr = 1;
    memset(File,0,sizeof(File));

    while(1)
    {
    	if(dataAvailable(0))
    	{
    		int ch;
    		ch=getch();
    		if(ch==0x03)		// exit on CTRL-C
    			break;
    		write(iDescriptor,&ch,1);
    	}
    	else
    	{
    		if(dataAvailable(iDescriptor))
    		{
    			int j;

    			i=read(iDescriptor, &data,sizeof(data));

    			for(j=0;j<i;j++)
    			{
					protocolHandler(data[j]);
    			}

    			usleep(1000);
    		}
    		else
    			usleep(5000);
    	}
    }

	return EXIT_SUCCESS;
}



int getcEsc(int data)
{
	static int escState = 0;
	int r;
	r=0;

	switch(data)
	{
	case 0x02:
	case 0x03:
	case 0x10:
	{
		if(escState)
		{
			r=data;
			escState=0;
		}
		else
		{
			escState=1;
			if(data!=0x10)
			{
				r=-2;
			}
			else
			{
				r=-1;
			}
		}
		break;
	}
	default:
	{
		r=data;
		break;
	}
	}
	return r;
}


void putPort(int data)
{
	int err;
	int errcnt;

	err=0;
	errcnt=0;

	do
	{
		if(err){
			usleep(1000);
			errcnt++;
		}
		err=write(iDescriptor,&data,1);
	}while(err==-1 && errno==EAGAIN && errcnt<100);
	if(errcnt)
	{
		fprintf(stderr,"Error writing to serial Port. Discarding some data.\n");
	}
	if(err==-1 && errno!=EAGAIN)
	{
		perror("Unrecoverable Error while writing to serial port. Exiting...\n");
		exit(errno);
	}
}


void putcEsc(int data)
{

	switch(data)
	{
	case 0x02:
	case 0x03:
	case 0x10:
		putPort(0x10);
	default:
		putPort(data);
	}
}


void putDwEsc(uint32_t data)
{
	int i;

	for(i=0;i<4;i++)
	{
		putcEsc(data>>24);
		data >>= 8;
	}
}


void putWEsc(uint16_t data)
{
	int i;

	for(i=0;i<2;i++)
	{
		putcEsc(data>>8);
		data >>= 8;
	}
}


void putBufEsc(char * buf, int len)
{
	int i;
	for(i=0;i<len;i++)
	{
		putcEsc(*buf++);
	}
}


void putsEsc(char * s)
{
	while(*s)
	{
		putcEsc(*s++);
	}
	putPort(0x03);
}


void putfiEsc(struct FileInfo * fi)
{
	union u_ftdu{
		t_ffdate fd;
		t_fftime ft;
		uint16_t i;
	} ftd;

	putWEsc(htobe16(fi->attr));

	ftd.i = htobe16(((union u_ftdu) (fi->LastWriteDate)).i);
	putWEsc(ftd.i);

	ftd.i = htobe16(((union u_ftdu) (fi->LastWriteTime)).i);
	putWEsc(ftd.i);

	putDwEsc(htobe32(fi->filesize));

	putBufEsc(fi->filename,sizeof(fi->filename));
}



void foundFile(struct dirent * dir)
{
	struct stat st;
	struct tm * time;
	struct FileInfo dirFile;

	if(stat(dir->d_name, &st))
	{
		time = localtime(&((st.st_mtimespec).tv_sec));
		dirFile.LastWriteDate.year = time->tm_year-80;
		dirFile.LastWriteDate.month = time->tm_mon+1;
		dirFile.LastWriteDate.day = time->tm_mday;
		dirFile.LastWriteTime.hour = time->tm_hour;
		dirFile.LastWriteTime.min = time->tm_min;
		dirFile.LastWriteTime.sek_2 = time->tm_sec / 2;

		dirFile.attr = (st.st_mode == S_IFDIR) ? 0x10 : 0x00;

		dirFile.filesize = (uint32_t) st.st_size;

		strncpy(dirFile.filename, dir->d_name, 13);
	}
	else
	{
		memset(&dirFile,0,sizeof(dirFile));
	}
	putfiEsc(&dirFile);
}


void protocolHandler(char c)
{
	static int state = STATE_IDLE;
	static int getArgument = GET_IDLE;
	static int cmd = -1;
	static char arg_str1[PATH_MAX];
	static char arg_str2[PATH_MAX];
	static uint32_t arg_dw;
	static uint16_t arg_w;
	static int iArg=0;
	static DIR * dirp=NULL;
	static int activeFptr;

	int i;
	int r;

	r=getcEsc(c);

	if(r==-1)
		return;

	if(c==0x02 && r==-2 && state != STATE_IDLE)
	{
		fprintf(stderr, "Received request while processing %02x. Aborting.\n",cmd );
		state = STATE_IDLE;
		cmd = -1;
	}


	switch(getArgument)
	{
	case GET_STRING1:
		if(r!=-2)
		{
			if(strlen(arg_str1)<(1+sizeof(arg_str1)) )
			{
				char c = (char) r;
				strncat(arg_str1, &c,1);
			}
		}
		else
		{
			i=0;
			getArgument = GET_IDLE;
			iArg++;
		}
		break;
	case GET_STRING2:
		if(r!=-2)
		{
			if(strlen(arg_str2)<(1+sizeof(arg_str2)) )
			{
				char c = (char) r;
				strncat(arg_str2, &c,1);
			}
		}
		else
		{
			i=0;
			getArgument = GET_IDLE;
			iArg++;
		}
		break;
	case GET_DW:
		if(r!=-2)
		{
			arg_dw |= (uint8_t) r;
			if(++i<4)
			{
				arg_dw <<= 8;
			}
			else
			{
				i=0;
				getArgument = GET_IDLE;
				iArg++;
			}
		}
		break;
	case GET_W:
		if(r!=-2)
		{
			arg_w |= (uint8_t) r;
			if(++i<2)
			{
				arg_w <<= 8;
			}
			else
			{
				i=0;
				getArgument = GET_IDLE;
				iArg++;
			}
		}
		break;
	case GET_FD:
		if(r!=-2)
		{
			activeFptr |= (uint8_t) r;
			if(++i<4)
			{
				activeFptr <<= 8;
			}
			else
			{
				i = 0;
				getArgument = GET_IDLE;
				iArg++;
			}
		}
		break;
	default:
		break;
	}


	if(getArgument != GET_IDLE)
		return;


	switch(state)
	{
	case STATE_IDLE:
	{
		if(r>=0)
			write(1,&r,1);		// print character in console
		else
		if(r==-2 && c==2)		// start command
		{
			fprintf(stderr, "Preparing for request\n,");
			state = STATE_GETCMD;
			iArg = 0;
		}
		break;
	}
	case STATE_GETCMD:
	{
		if(r>= CMD_FOPEN && r<=CMD_UNGETC)
		{
			putPort(0x03);
			cmd = r;
			state = STATE_PROCESS;
			switch(cmd)
			{
			case CMD_FOPEN:
			case CMD_FINDFIRST:
			case CMD_REMOVE:
			case CMD_RENAME:
				state = STATE_PROCESS;
				getArgument = GET_STRING1;
				memset(arg_str1,0,sizeof(arg_str1));
				memset(arg_str2,0,sizeof(arg_str2));
				break;
			case CMD_FWRITE:
			case CMD_FCLOSE:
			case CMD_FGETC:
			case CMD_FPUTC:
			case CMD_FGETS:
			case CMD_FPUTS:
			case CMD_FTELL:
			case CMD_FSEEK:
				state = STATE_PROCESS;
				getArgument = GET_FD;
				i=0;
				activeFptr = 0;
				break;
			case CMD_FREAD:
				state = STATE_PROCESS;
				getArgument = GET_DW;
				i=0;
				arg_dw = 0;
				break;
			case CMD_UNGETC:
				state = STATE_PROCESS;
				getArgument = GET_W;
				i=0;
				arg_w = 0;
				break;
			default:
				state = STATE_IDLE;
				cmd = -1;
			}
		}
		else
		{
			fprintf(stderr, "Ignoring unknown request 0x%02x\n",r );
			state = STATE_IDLE;
		}
		break;
	}
	case STATE_PROCESS:
	{
		switch(cmd)
		{
		case CMD_FOPEN:
			if(iArg==1)
			{
				getArgument = GET_STRING2;
			}
			else
			{
				char * s;
				char * a=NULL;
				struct stat st;

				s=arg_str1;
				// replace \ by /
				do
				{
					s=strchr(arg_str1,'\\');
					if(s)
						*s='/';
				}while(s);

				s=strrchr(arg_str1,'/');	// restrict access to current directory
				if(s==NULL)
					s=arg_str1;

				a=strchr(arg_str2, 'w');
				if(!a)
				{
					a=strchr(arg_str2, 'W');
				}

				if(stat(s, &st)==0 && (a!=NULL))
				{
					printf("File %s exists. Ignoring 'open for write' request.\n",s);
					activeFptr = 0;
				}
				else
				{
					activeFptr=fptr;
					if (File[activeFptr-1] == NULL)
					{
						File[activeFptr-1] = fopen(s, arg_str2);	// open file
						printf("File %s opened in mode %s.", s, arg_str2);
					}
					else
						activeFptr=0;
				}
				putDwEsc(activeFptr);

				if(++fptr>MAXFPTR)
					fptr = 1;

				state = STATE_IDLE;
			}
			break;
		case CMD_FCLOSE:
		{
			int res;
			res=fclose(File[activeFptr-1]);
			putWEsc((uint16_t) res);
			state = STATE_IDLE;
			break;
		}
		case CMD_FREAD:
		{
			int d;
			if(iArg==1)
			{
				getArgument = GET_FD;
			}
			else
			{
				while(arg_dw--)
				{
					d=fgetc(File[activeFptr-1]);
					if(d!=EOF)
					{
						putcEsc(d);
					}
					else
					{
						putPort(0x03);
					}
				}
				state = STATE_IDLE;
			}
			break;
		}
		case CMD_FWRITE:
		{
			if(r==-2)
			{
				if(c!=3)
				{
					printf("Protocol exception: Received 0x02 during fwrite. Halting operation.\n");
				}
				state = STATE_IDLE;
			}
			else
			{
				fputc(r, File[activeFptr-1]);
			}
			break;
		}
		case CMD_FGETC:
		{
			int c;
			c=fgetc(File[activeFptr-1]);
			putWEsc((uint16_t)c);
			state = STATE_IDLE;
			break;
		}
		case CMD_FPUTC:
		{
			if(iArg==1)
			{
				getArgument = GET_W;
			}
			else
			{
				int res;

				res=fputc((int) arg_w, File[activeFptr-1]);
				putWEsc((uint16_t)res);
				state = STATE_IDLE;
			}
			break;
		}
		case CMD_FGETS:
		{
			if(iArg==1)
			{
				getArgument = GET_W;
			}
			else
			{
				char cbuf[4096];

				if(arg_w >4096)
				{
					putWEsc(0);
				}
				else
				{
					if(fgets(cbuf, (int) arg_w, File[activeFptr-1]))
					{
						putWEsc(1);
						putsEsc(cbuf);
					}
					else
					{
						putWEsc(0);
					}
					state = STATE_IDLE;
				}
			}
			break;
		}
		case CMD_FPUTS:
		{
			if(iArg==1)
			{
				getArgument = GET_STRING1;
			}
			else
			{
				int res;
				res = fputs(arg_str1, File[activeFptr-1]);
				putWEsc((uint16_t)res);
				state = STATE_IDLE;
			}
			break;
		}
		case CMD_FINDFIRST:
		{
			if(iArg==1)
			{
				getArgument = GET_W;
			}
			else
			{
				struct dirent * dir;

				if(dirp)
					closedir(dirp);

				dirp = opendir(".");
				if(dirp && (dir = readdir(dirp)))
				{
					dir = readdir(dirp);
					putWEsc(0);
					foundFile(dir);
				}
				else
				{
					putWEsc(-1);
				}
				state = STATE_IDLE;
			}
			break;
		}
		case CMD_FINDNEXT:
		{
			struct dirent * dir;

			if((dir = readdir(dirp)))
			{
				putWEsc(0);
				foundFile(dir);
			}
			else
			{
				putWEsc(-1);
				closedir(dirp);
			}
			state = STATE_IDLE;
			break;
		}
		case CMD_REMOVE:
		{
			fprintf(stderr,"Request to remove file ignored. (unimplemented)\n.");
			fprintf(stderr,"Please remove %s manually\n",arg_str1);
			state = STATE_IDLE;
			break;
		}
		case CMD_RENAME:
			if(iArg==1)
			{
				getArgument = GET_STRING2;
			}
			else
			{
				fprintf(stderr,"Request to rename file ignored. (unimplemented)\n.");
				fprintf(stderr,"Please rename\n%s\nmanually to\n%s\n",arg_str1, arg_str2);
				state = STATE_IDLE;
			}
			break;
		case CMD_FTELL:
		{
			long l;
			l = ftell(File[activeFptr-1]);
			putDwEsc((uint32_t) l);
			state = STATE_IDLE;
			break;
		}
		case CMD_FSEEK:
		{
			if(iArg==1)
			{
				getArgument = GET_DW;
			}
			else
			if(iArg==2)
			{
				getArgument = GET_W;
			}
			else
			{
				putWEsc((uint16_t) fseek(File[activeFptr-1], arg_dw, arg_w));
				state = STATE_IDLE;
			}
			break;
		}
		case CMD_UNGETC:
		{
			if(iArg==1)
			{
				getArgument = GET_STRING1;
			}
			else
			{
				putWEsc((uint16_t) ungetc((int)arg_w, File[activeFptr-1]));
				state = STATE_IDLE;
			}
			break;
		}
		default:
		{
			fprintf(stderr,"Ignoring unimplemented request 0x%02x.\n", cmd);
			state = STATE_IDLE;
		}
		}
	}
	}
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
