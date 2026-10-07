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
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
#include <strings.h>

#ifdef _WIN32
#include <direct.h>		// getcwd
#include <io.h>			// unlink
#include <fcntl.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <sys/syslimits.h>
#endif

#include "platform.h"

#define DEFAULT_BITRATE 19200;

#ifndef OPENRS_VERSION
#define OPENRS_VERSION "dev"	// release builds set this from the tag
#endif

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

#define MAXFPTR 256
FILE * File[MAXFPTR];	// since TNC3OS does not support 64 Bit pointers, but
					// wants to handle "File *" by itself, we do a mapping
					// using a table. Instead of File * we return a table
					// index
int fptr;
char * cwd = NULL;
char * wd = NULL;


void protocolHandler(char c);
void putcEsc(int data);

void restoreState(void)
{
	int i;

	fprintf(stdout,"\n\rExiting...\n\r");
	fflush(stdout);
	serialClose();
	consoleRestore();

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
	unsigned char data[1024];
	int i;
	int rtscts = 0;
	int consoleOpen = 1;

	// options must precede the positional arguments
	while(argc > 1 && argv[1][0] == '-')
	{
		if(!strcmp(argv[1], "-r") || !strcmp(argv[1], "--rtscts"))
		{
			rtscts = 1;
		}
		else
		{
			fprintf(stderr, "Unknown option %s\r\n", argv[1]);
			exit(1);
		}
		argc--;
		argv++;
	}

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
		printf("\nOpenRS " OPENRS_VERSION "\r\n");
		printf("Please specify serial device and (optionally) speed (default: 19200).\r\n");
		printf("Usage: openrs [-r] <serialPort> <speed> <tnc command>\r\n");
		printf("  -r, --rtscts  enable RTS/CTS hardware flow control.\r\n");
		printf("                Only use with a cable carrying the handshake lines,\r\n");
		printf("                otherwise nothing will be sent to the TNC.\r\n");
		printf("Exit with CTRL-C\r\n\r\n");
		printf("!!! Use DOS/Windows style drive letters as prefix to read from TNC to a local file\n\r");
		printf("    otherwise the TNC will not initiate the transfer.\n\r");
		printf("The drive letter will be stripped and the file placed in the current directory.\r\n");
		printf("Example:\nopenrs /dev/tty.usb 19200 cp r:dip1.scr c:dip1.scr\r\n\r\n");
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

#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);	// messages already end in \r\n
#endif

	atexit(restoreState);
	signal(SIGINT,restoreStateSig);
	signal(SIGTERM,restoreStateSig);

	if(serialOpen(port, bitrate, rtscts)!=0)
	{
		exit(1);
	}

	consoleRaw();

	fptr = 1;
	memset(File,0,sizeof(File));

	while(1)
	{
		int ch = consoleOpen ? consoleRead() : CONSOLE_NO_KEY;

		if(ch==CONSOLE_EOF)
		{
			consoleOpen = 0;	// keep serving the TNC without a keyboard
		}
		else
		if(ch>=0)
		{
			if(ch==0x03)		// exit on CTRL-C
				break;
			if(ch==0x7f)
				ch=0x08;		// replace DEL by BS
			putcEsc(ch);
			continue;
		}

		i=serialRead(data, sizeof(data));
		if(i<0)
		{
			fprintf(stderr, "Error reading from serial port.\r\n");
			exit(1);
		}
		if(i>0)
		{
			int j;

			for(j=0;j<i;j++)
			{
				protocolHandler(data[j]);
			}
			sleepMs(1);
		}
		else
			sleepMs(5);
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
	if(serialWrite((unsigned char) data) < 0)
	{
		fprintf(stderr, "Exiting...\r\n");
		exit(1);
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
	//no break -> escape, then data
	default:
		putPort(data);
		break;
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

	memset(&dirFile,0,sizeof(dirFile));

	name = malloc(strlen(wd)+strlen(dir->d_name)+1);
	if(name==NULL)
	{
		perror("Error allocating memory.\r\n");
		exit(1);
	}
	sprintf(name,"%s%s",wd, dir->d_name);

	if(stat(name, &st)==0)
	{
		time = localtime(&st.st_mtime);
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
		strncpy(dirFile.filename, dir->d_name, 13);
	}
	putfiEsc(&dirFile);
	free(name);
}


int sanitizePath(char * dirtyPath, char * cleanPath, size_t cleanPathMaxLen)
{
	char * s;
	char * d;
	char * c;
	int dplen;
	int len = 0;

	if(cleanPathMaxLen < 1)
		return -1;

	dplen = strlen(dirtyPath);

	if(dplen>cleanPathMaxLen)
	{
		return -1;
	}

	if(dplen == 0)
	{
		*cleanPath = 0;
		return 0;
	}

	d = cleanPath;
	s = dirtyPath;
	strncpy(d,s,cleanPathMaxLen-1);
	// ensure string is terminated
	d[cleanPathMaxLen-1]=0;

	// replace \ by /
	while((c = strchr(d,'\\') ))
	{
		*c = '/';
	}

	// remove drive & :
	if( (c=strchr(d,':')) )
	{
		len = c-d;
		if(len<3)
		{
			memmove(d,c+1,cleanPathMaxLen-len-1);
		}
	}
	return 0;
}


/*
int findFirst(char * name, int attribute)
{
	struct dirent * dir;
	char * cc;
	char * cd;
	char * cs;
	size_t cslen;
	int listdir;

	listdir=0;

	cslen = strlen(name)*2;
	cs = malloc(cslen);

	if(dir)
		closedir(dir);

	sanitizePath(name, cs, cslen);

	cd = strstr(cs,"*.*");
	if(cd)
	{
		// list directory
		*cd = 0;
		listdir = 1;
	}


	if(listdir)
	{
		sprintf(wd,"%s/%s",cwd,cc);
		dir = opendir(wd);
		if(dir && (dir = readdir(dir)))
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

			strncpy(dirFile.filename, name, 13);
			putWEsc(0);
			putfiEsc(&dirFile);
		}
		else
		{
			putWEsc(-1);
		}
	}

}



int findNext(void)
{

}
*/


/* DOS style wildcard match ('*', '?'), case-insensitive like the TNC */
int wildcardMatch(const char * p, const char * s)
{
	for(; *p; p++, s++)
	{
		if(*p=='*')
		{
			while(*++p=='*')
				;
			if(!*p)
				return 1;
			for(; *s; s++)
			{
				if(wildcardMatch(p, s))
					return 1;
			}
			return 0;
		}
		if(!*s)
			return 0;
		if(*p!='?' && toupper((unsigned char) *p)!=toupper((unsigned char) *s))
			return 0;
	}
	return *s==0;
}


/* next directory entry matching pattern, "*.*" matches all (DOS semantics) */
struct dirent * findNextMatch(DIR * d, const char * pattern)
{
	struct dirent * e;

	while((e=readdir(d)))
	{
		if(!strcmp(pattern, "*.*") || wildcardMatch(pattern, e->d_name))
			return e;
	}
	return NULL;
}


/* map DOS path from TNC to a file name in the current directory.
   Like the TNC file system, lookup is case-insensitive but case-preserving:
   an existing file is used with its actual spelling (exact match preferred),
   a new file keeps the spelling sent by the TNC. */
char * localFileName(char * dosPath, char * buf, size_t len)
{
	char * s;
	DIR * d;
	struct dirent * e;

	if(sanitizePath(dosPath, buf, len))
	{
		buf[0]=0;
	}
	fprintf(stderr, "Sanitized Path: %s\r\n", buf);

	s=strrchr(buf,'/');	// restrict access to current directory
	if(s==NULL)
		s=buf;
	else
		s++;

	// look the name up in the directory rather than with access(): on a
	// case-insensitive file system (macOS) access() succeeds for any spelling
	if(*s && (d=opendir(".")))
	{
		char * match = NULL;

		while((e=readdir(d)))
		{
			if(strcmp(e->d_name, s)==0)
			{
				free(match);	// exact match, keep the name
				match = NULL;
				break;
			}
			if(!match && strcasecmp(e->d_name, s)==0)
			{
				match = strdup(e->d_name);
			}
		}
		if(match)
		{
			memcpy(s, match, strlen(s));	// same length
			free(match);
		}
		closedir(d);
	}

	fprintf(stderr, "restricted path: %s\r\n", s);
	return s;
}


/* map TNC file handle (1..MAXFPTR) to FILE *, NULL if invalid or not open */
FILE * getFile(int fd)
{
	if(fd<1 || fd>MAXFPTR)
		return NULL;
	return File[fd-1];
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
#ifdef DEBUG
	static int bc;
#endif
	static int listdir=0;
	static char findPattern[PATH_MAX];

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
			if(strlen(arg_str1)<(sizeof(arg_str1)-1) )
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
			if(strlen(arg_str2)<(sizeof(arg_str2)-1) )
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
		if(r>=0){
			fflush(stdout);	// keep order with printf() messages
			if(!consoleWrite((unsigned char) r))	// print character in console
			{
				fprintf(stderr, "Error writing to STDOUT.\r\n");
				exit(errno);
			}
		}
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
	// no break
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
				char local_path[PATH_MAX];

				s=localFileName(arg_str1, local_path, sizeof local_path);

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
						File[activeFptr-1] = NULL;
					}
					FILE * f;
					f = fopen(s, arg_str2);	// open file
					if(f)
					{
						File[activeFptr-1] = f;
						printf("File %s opened in mode %s.\r\n", s, arg_str2);
#ifdef DEBUG
						bc = 0;
#endif
					}
					else
					{
						activeFptr=0;
						printf("File open error for %s:\r\n", s);
						printf("%s\n\r",strerror(errno));
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
			FILE * f = getFile(activeFptr);
			if(f)
			{
				res=fclose(f);
				File[activeFptr-1] = NULL;
			}
			else
			{
				res=EOF;
			}
			putWEsc((uint16_t) res);
			state = STATE_IDLE;
			break;
		}
		case CMD_FREAD:
		{
			int d;
			FILE * f;
			if(iArg==1)
			{
				getArgument = GET_FD;
			}
			else
			{
				f = getFile(activeFptr);
				while(arg_dw--)
				{
					d = f ? fgetc(f) : EOF;
					if(d==EOF)
					{
						putPort(0x03);	// short read, end data with one ETX
						break;
					}
					putcEsc(d);
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
				if(getFile(activeFptr))
				{
					fputc(r, getFile(activeFptr));
				}
			}
			break;
		}
		case CMD_FGETC:
		{
			int c;
			if(getFile(activeFptr))
			{
				c=fgetc(getFile(activeFptr));
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

				if(getFile(activeFptr))
				{
					res=fputc((int) arg_w, getFile(activeFptr));
				}
				else
				{
					res=EOF;
				}
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

				if((arg_w > 4096) || (getFile(activeFptr)==NULL))
				{
					putWEsc(0);
				}
				else
				{
					if(fgets(cbuf, (int) arg_w, getFile(activeFptr)))
					{
						putWEsc(1);
						putsEsc(cbuf);
					}
					else
					{
						putWEsc(0);
					}
				}
				state = STATE_IDLE;
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
				if(getFile(activeFptr))
				{
					res = fputs(arg_str1, getFile(activeFptr));
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
				char local_path[PATH_MAX];
				char * cc;
				char * cd;

				listdir=0;

				if(dirp)
				{
					closedir(dirp);
					dirp=NULL;
				}

				// strip drive letter (if any) and replace \ by /
				if(sanitizePath(arg_str1, local_path, sizeof local_path))
				{
					local_path[0]=0;
				}

				// path is relative to current directory
				cc = local_path;
				while(*cc=='/')
				{
					cc++;
				}
				fprintf(stderr, "Sanitized Path: %s\r\n", cc);

				cd = strrchr(cc,'/');
				cd = cd ? cd+1 : cc;
				if(strpbrk(cd,"*?"))
				{
					// list directory, cc keeps the directory part
					strcpy(findPattern, cd);
					*cd = 0;
					listdir = 1;
				}

				if(listdir)
				{
					sprintf(wd,"%s/%s",cwd,cc);
					dirp = opendir(wd);
					if(dirp && (dir = findNextMatch(dirp, findPattern)))
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

					// same name mapping as FOPEN
					cc=localFileName(arg_str1, local_path, sizeof local_path);

					if( (stat(cc, &st)==0) && (!S_ISDIR(st.st_mode)))
					{
						time = localtime(&st.st_mtime);
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

						strncpy(dirFile.filename, cc, 13);
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

			if(listdir && dirp && (dir = findNextMatch(dirp, findPattern)))
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
			char local_path[PATH_MAX];
			char * s;
			struct stat st;
			int res = -1;

			// same name mapping as FOPEN, regular files only
			s=localFileName(arg_str1, local_path, sizeof local_path);
			if((stat(s, &st)==0) && S_ISREG(st.st_mode))
			{
				res=unlink(s);
			}
			if(res==0)
			{
				printf("File %s removed.\r\n", s);
			}
			else
			{
				printf("Could not remove %s.\r\n", s);
			}
			putWEsc((uint16_t) res);
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
			if(getFile(activeFptr))
			{
				l = ftell(getFile(activeFptr));
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
				if(getFile(activeFptr))
				{
					putWEsc((uint16_t) fseek(getFile(activeFptr), (long)(int32_t) arg_dw, arg_w));
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
				getArgument = GET_FD;
			}
			else
			{
				if(getFile(activeFptr))
				{
					putWEsc((uint16_t) ungetc((int)arg_w, getFile(activeFptr)));
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
			break;
		}
		}
		break;
	}
	}
}
