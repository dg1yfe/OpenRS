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
#include <ctype.h>

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
	t_fftime	LastWriteTime;
	t_ffdate	LastWriteDate;
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
char * cwd = NULL;
char * wd = NULL;


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
	if(cwd)
	{
		free(cwd);
		cwd=NULL;
	}
	if(wd)
	{
		free(wd);
		wd=NULL;
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
    	if(c==0x7f)
    	{
    		c=0x08;		// replace DEL by BS
    	}
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
			fprintf(stderr, "Invalid device name. Name exceeds %d bytes (PATH_MAX)\r\n" ,PATH_MAX);
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
				fprintf(stderr, "Could not parse bitrate. Argument 2 ignored.\r\n");
				fprintf(stderr, "Bitrate defaults to %d bps.\r\n", bitrate);
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
				printf("Sorry, could not allocate memory for commands.\nExiting...\r\n");
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
		printf("\nPlease specify serial device and (optionally) speed (default: 19200).\r\n");
		printf("Usage: openrs <serialPort> <speed> <tnc command>\r\n");
		printf("Exit with CTRL-C\r\n\r\n");
		printf("Example:\nopenrs /dev/tty.usb 19200 flash epflash.bin\r\n\r\n");
		exit(0);
	}

	cwd = getcwd(NULL, 0);
	if(cwd==NULL)
	{
		perror("Error when calling getcwd.\r\n");
		exit(1);
	}

	wd = malloc(strlen(cwd)+1+PATH_MAX);

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



int getcEsc(char data)
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
			r = (unsigned char) data;
			escState=0;
		}
		else
		{
			if(data!=0x10)
			{
				r=-2;
			}
			else
			{
				escState=1;
				r=-1;
			}
		}
		break;
	}
	default:
	{
		r = (unsigned char) data;
		escState=0;
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
		fprintf(stderr,"Error writing to serial Port. Discarding some data.\r\n");
	}
	if(err==-1 && errno!=EAGAIN)
	{
		perror("Unrecoverable Error while writing to serial port. Exiting...\r\n");
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
		data <<= 8;
	}
}


void putWEsc(uint16_t data)
{
	int i;

	for(i=0;i<2;i++)
	{
		putcEsc(data>>8);
		data <<= 8;
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

	putWEsc(fi->attr);

	ftd.i = ((union u_ftdu) (fi->LastWriteTime)).i;
	putWEsc(ftd.i);

	ftd.i = ((union u_ftdu) (fi->LastWriteDate)).i;
	putWEsc(ftd.i);

	putDwEsc(fi->filesize);

	putBufEsc(fi->filename,sizeof(fi->filename));
}



void foundFile(struct dirent * dir)
{
	struct stat st;
	struct tm * time;
	struct FileInfo dirFile;
	char * name;

	name = malloc(sizeof(dir->d_name)+sizeof(wd));
	sprintf(name,"%s%s",wd, dir->d_name);

	if(stat(name, &st)==0)
	{
		time = localtime(&((st.st_mtimespec).tv_sec));
		dirFile.LastWriteDate.year = time->tm_year-80;
		dirFile.LastWriteDate.month = time->tm_mon+1;
		dirFile.LastWriteDate.day = time->tm_mday;
		dirFile.LastWriteTime.hour = time->tm_hour;
		dirFile.LastWriteTime.min = time->tm_min;
		dirFile.LastWriteTime.sek_2 = time->tm_sec / 2;

		dirFile.attr = 0;
		if(S_ISDIR(st.st_mode))
		{
			dirFile.attr = 0x10;
		}

		dirFile.filesize = (uint32_t) st.st_size;

		strncpy(dirFile.filename, dir->d_name, 13);
	}
	else
	{
		memset(&dirFile,0,sizeof(dirFile));
		strncpy(dirFile.filename, dir->d_name, 13);
	}
	putfiEsc(&dirFile);
	free(name);
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
	static int i;
	static int bc;
	static int listdir=0;

	int r;

	r=getcEsc(c);

#ifdef DEBUG
	if(r==-2)
	{
		fprintf(stderr,"\r\n%02X\r\n", (uint8_t) c);
		bc=0;
	}
	else
	if(r>=0)
	{
		if(bc++ % 16 == 0)
		{
			fprintf(stderr,"\r\n");
		}
		fprintf(stderr,"%02hhx ",(uint8_t) c);
	}
#endif

	if(r==-1)
		return;

	if(c==0x02 && r==-2 && state != STATE_IDLE)
	{
		printf("Received request while processing %02x. Aborting.\r\n",cmd );
		state = STATE_IDLE;
		cmd = -1;
		return;
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
			fprintf(stderr, "Argument 1 (String): %s\r\n", arg_str1);
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
			fprintf(stderr, "Argument 2 (String): %s\r\n", arg_str2);
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
				fprintf(stderr, "\r\nArgument (DWORD): 0x%04x\r\n", arg_dw);
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
				fprintf(stderr, "\r\nArgument (WORD): 0x%02x\r\n", arg_w);
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
				fprintf(stderr, "\r\nArgument (FD *): 0x%x\r\n", activeFptr);
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
			fprintf(stderr, "Preparing for request\r\n");
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
			iArg = 0;
			i = 0;
			activeFptr = 0;
			arg_dw = 0;
			arg_w  = 0;
			memset(arg_str1,0,sizeof(arg_str1));
			memset(arg_str2,0,sizeof(arg_str2));

			cmd = r;
			state = STATE_PROCESS;
			fprintf(stderr, "Received request 0x%02x.\r\n",cmd);
			switch(cmd)
			{
				case CMD_FOPEN:
				case CMD_FINDFIRST:
				case CMD_REMOVE:
				case CMD_RENAME:
					state = STATE_PROCESS;
					getArgument = GET_STRING1;
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
					break;
				case CMD_FREAD:
					state = STATE_PROCESS;
					getArgument = GET_DW;
					break;
				case CMD_UNGETC:
					state = STATE_PROCESS;
					getArgument = GET_W;
					break;
				case CMD_FINDNEXT:
					break;
				default:
					state = STATE_IDLE;
					cmd = -1;
			}
		}
		else
		{
			fprintf(stderr, "Ignoring unknown request 0x%02x\r\n",r );
			state = STATE_IDLE;
			break;
		}
		if(getArgument != GET_IDLE)
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
				while(*s)
				{
					*s=tolower(*s);
					s++;
				}

				if(strlen(arg_str1)>3)
				{
					size_t l = strlen(arg_str1);

					memmove(arg_str1, &arg_str1[3],l-3);
					arg_str1[l-3]=0;
				}
				s=arg_str1;
				// replace \ by /
				while((s=strchr(arg_str1,'\\')))
				{
					*s='/';
				};

				s=strrchr(arg_str1,'/');	// restrict access to current directory
				if(s==NULL)
					s=arg_str1;

				if((strlen(arg_str1)>1) && (arg_str1[1]==':'))
					s=&arg_str1[2];

				a=strchr(arg_str2, 'w');
				if(!a)
				{
					a=strchr(arg_str2, 'W');
				}

				if((stat(s, &st)==0) && (a!=NULL))
				{
					printf("File %s exists. Ignoring 'open for write' request.\r\n",s);
					activeFptr = 0;
				}
				else
				{
					activeFptr=fptr;
					if (File[activeFptr-1] != NULL)
					{
						fclose(File[activeFptr-1]);
					}
					FILE * f;
					f = fopen(s, arg_str2);	// open file
					if(f)
					{
						File[activeFptr-1] = f;
						printf("File %s opened in mode %s.\r\n", s, arg_str2);
						bc = 0;
					}
					else
					{
						activeFptr=0;
						printf("File %s not found.\r\n", s);
					}
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
			// begin processing with first data byte (the next one)
			if(i==0)
			{
				i++;
				break;
			}

			if(r==-2)
			{
				if(c!=3)
				{
					fprintf(stderr,"\r\n-x-\r\n");
					printf("Protocol exception: Received 0x02 during fwrite. Halting operation.\r\n");
				}
				else
				{
					fprintf(stderr,"\r\n---\r\n");
				}
				state = STATE_IDLE;
			}
			else
			{
				if(File[activeFptr-1])
				{
					fputc(r, File[activeFptr-1]);
				}
			}
			break;
		}
		case CMD_FGETC:
		{
			int c;
			if(File[activeFptr-1])
			{
				c=fgetc(File[activeFptr-1]);
				putWEsc((uint16_t)c);
			}
			else
			{
				putWEsc(EOF);
			}
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

				if((arg_w > 4096) || (File[activeFptr-1]==NULL))
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
				if(File[activeFptr-1])
				{
					res = fputs(arg_str1, File[activeFptr-1]);
				}
				else
				{
					res = EOF;
				}
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
				char * cc;
				char * cd;

				listdir=0;

				if(dirp)
					closedir(dirp);

				if(strlen(arg_str1)>3)
				{
					cc = &arg_str1[3];
				}
				else
					cc = arg_str1;

				while((cd = strchr(cc,'\\') ))
				{
					*cd = '/';
				}

				cd = strstr(cc,"*.*");
				if(cd)
				{
					// list directory
					*cd = 0;
					listdir = 1;
				}

				if(listdir)
				{
					sprintf(wd,"%s/%s",cwd,cc);
					dirp = opendir(wd);
					if(dirp && (dir = readdir(dirp)))
					{
						putWEsc(0);
						foundFile(dir);
					}
					else
					{
						putWEsc(-1);
					}
				}
				else
				{
					struct stat st;
					struct tm * time;
					struct FileInfo dirFile;

					memset(&dirFile,0,sizeof(dirFile));

					if( (stat(cc, &st)==0) && (!S_ISDIR(st.st_mode)))
					{
						time = localtime(&((st.st_mtimespec).tv_sec));
						dirFile.LastWriteDate.year = time->tm_year-80;
						dirFile.LastWriteDate.month = time->tm_mon+1;
						dirFile.LastWriteDate.day = time->tm_mday;
						dirFile.LastWriteTime.hour = time->tm_hour;
						dirFile.LastWriteTime.min = time->tm_min;
						dirFile.LastWriteTime.sek_2 = time->tm_sec / 2;

						dirFile.attr = 0;
						if(S_ISDIR(st.st_mode))
						{
							dirFile.attr = 0x10;
						}

						dirFile.filesize = (uint32_t) st.st_size;

						strncpy(dirFile.filename, arg_str1, 13);
						putWEsc(0);
						putfiEsc(&dirFile);
					}
					else
					{
						putWEsc(-1);
					}
				}
				state = STATE_IDLE;
			}
			break;
		}
		case CMD_FINDNEXT:
		{
			struct dirent * dir;

			if(listdir && (dir = readdir(dirp)))
			{
				putWEsc(0);
				foundFile(dir);
			}
			else
			{
				putWEsc(-1);
				if(dirp)
				{
					closedir(dirp);
					dirp=NULL;
				}
			}
			state = STATE_IDLE;
			break;
		}
		case CMD_REMOVE:
		{
			fprintf(stderr,"Request to remove file ignored. (unimplemented)\r\n.");
			fprintf(stderr,"Please remove %s manually\r\n",arg_str1);
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
				fprintf(stderr,"Request to rename file ignored. (unimplemented)\r\n.");
				fprintf(stderr,"Please rename\r\n%s\nmanually to\r\n%s\r\n",arg_str1, arg_str2);
				state = STATE_IDLE;
			}
			break;
		case CMD_FTELL:
		{
			long l;
			if(File[activeFptr-1])
			{
				l = ftell(File[activeFptr-1]);
			}
			else
			{
				l = -1;
			}
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
				if(File[activeFptr-1])
				{
					putWEsc((uint16_t) fseek(File[activeFptr-1], arg_dw, arg_w));
				}
				else
				{
					putWEsc(EOF);
				}
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
				if(File[activeFptr-1])
				{
					putWEsc((uint16_t) ungetc((int)arg_w, File[activeFptr-1]));
				}
				else
				{
					putWEsc(EOF);
				}
				state = STATE_IDLE;
			}
			break;
		}
		default:
		{
			fprintf(stderr,"Ignoring unimplemented request 0x%02x.\r\n", cmd);
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
		printf("Error: can't open device %s\r\n", port);
		printf("       (%s)\r\n", strerror(errno));
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
                printf("Error: can't get actual settings for device %s\r\n", port);
                printf("       (%s)\r\n", strerror(errno));
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
                printf("Error: can't set input bitrate on %s\r\n", port);
                printf("       (%s)\r\n", strerror(errno));
            }

            /* Empfangsparameter setzen */
            if (cfsetospeed(&(wrk_termios), speed) == -1)
            {
                iError = 4;
                printf("Error: can't set output bitrate on %s\r\n", port);
                printf("       (%s)\r\n", strerror(errno));
            }
#ifndef __APPLE__
            if (speed == B38400)              /* wenn >= 38400 Bd     */
            {
                ser_io.flags &= ~ASYNC_SPD_MASK;      /* Speed-Flag -> 0      */
                ser_io.flags |= speedflag;      /* Speed-Flag setzen    */

                if (ioctl(NewRing->iDescriptor, TIOCSSERIAL, &ser_io) < 0)
                {
                    iError = 4;
                    printf("Error: can't set device settings on port %s\r\n", port);
                    printf("       (%s)\r\n", strerror(errno));
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
