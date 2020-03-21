/* atty - audio interface and driver for terminals
 * Copyright (C) 2020 Øyvind Kolås <pippin@gimp.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>. 
 */


#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <pty.h>

int atty_engine (void);
int vt_a85enc (const void *srcp, char *dst, int count);
int vt_a85dec (const char *src, char *dst, int count);
int vt_a85len (const char *src, int count);
int vt_base642bin (const char    *ascii,
                   int           *length,
                   unsigned char *bin);
void
vt_bin2base64 (const void *bin,
               int         bin_length,
               char       *ascii);

typedef struct AudioState {
  int action;
  int samplerate; // 8000
  int channels;   // 1
  int bits;       // 8
  int type;       // 'u'    u-law  f-loat  s-igned u-nsigned

  int mic;        // <- should 
                  //    request permisson,
                  //    and if gotten, start streaming
                  //    audio packets in the incoming direction
  int encoding;   // 'a' ascci85 'b' base64
  int compression; // unused for now, z zlib o opus

  int frames;

  uint8_t *data;
  int      data_size;
} AudioState;

typedef struct VtPty {
  int        pty;
  pid_t      pid;
} VtPty;

typedef struct _MrgVT MrgVT;

struct _MrgVT {
  char     *title;
  void    (*state)(MrgVT *vt, int byte);
  int       bell;
#define MAX_ARGUMENT_BUF_LEN (8192 + 16)

  char       *argument_buf;
  int        argument_buf_len;
  int        argument_buf_cap;
  ssize_t(*write)(void *serial_obj, const void *buf, size_t count);
  ssize_t(*read)(void *serial_obj, void *buf, size_t count);
  int    (*waitdata)(void *serial_obj, int timeout);
  void  (*resize)(void *serial_obj, int cols, int rows, int px_width, int px_height);

  VtPty      vtpty;

  AudioState audio;
};

static ssize_t vt_write (MrgVT *vt, const void *buf, size_t count)
{
  if (!vt->write) return 0;
  return vt->write (&vt->vtpty, buf, count);
}
static ssize_t vt_read (MrgVT *vt, void *buf, size_t count)
{
  if (!vt->read) return 0;
  return vt->read (&vt->vtpty, buf, count);
}
static int vt_waitdata (MrgVT *vt, int timeout)
{
  if (!vt->waitdata) return 0;
  return vt->waitdata (&vt->vtpty, timeout);
}

/* NOTE : the reason the source is split the way it is, is that the
 *        audio-engine originates in another project
 */
#include "vt-audio.h"

static struct termios orig_attr; /* in order to restore at exit */
static int    nc_is_raw = 0;

int tty_fd = STDIN_FILENO;

static void _nc_noraw (void)
{
  if (nc_is_raw && tcsetattr (tty_fd, TCSAFLUSH, &orig_attr) != -1)
    nc_is_raw = 0;
}
int has_data (int fd, int delay_ms);

static void
at_exit_mic (void)
{
  fprintf(stderr, "\033_Am=0;\e\\");

  while (has_data (STDIN_FILENO, 100))
  {
    char c;
    read (STDIN_FILENO, &c, (size_t)1);
  }
  _nc_noraw();
  fflush (NULL);
  usleep (1000 * 100);
}

static void
at_exit_speaker (void)
{
  _nc_noraw();
  fflush (NULL);
}

static int _nc_raw (void)
{
  struct termios raw;
  if (nc_is_raw)
    return 0;
  if (!isatty (tty_fd))
    return -1;
  if (tcgetattr (tty_fd, &orig_attr) == -1)
    return -1;
  raw = orig_attr;
  cfmakeraw (&raw);
  if (tcsetattr (tty_fd, TCSANOW, &raw) < 0)
    return -1;
  nc_is_raw = 1;
  return 0;
}


static long int atty_ticks (void)
{
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static unsigned char buf[BUFSIZ];

void
signal_int_speaker (int signum)
{
  at_exit_speaker ();
  exit (0);
}

void
signal_int_mic (int signum)
{
  at_exit_mic ();
  exit (0);
}

int buffered_samples = 0;
int sample_rate = 8000;
int bits = 8;
int channels = 1;
int ulaw = 1;
int compression = '0';
int encoding = '0';
int type = 'u';
int buffer_samples = 512;
int lost_time = 0;
int lost_start;
int lost_end;

enum {
  ACTION_STATUS  = 0,
  ACTION_RESET,
  ACTION_SPEAKER,
  ACTION_MIC,
  ACTION_ENGINE,
};

int action = ACTION_STATUS;

int has_data (int fd, int delay_ms)
{
  struct timeval tv;
  int retval;
  fd_set rfds;

  FD_ZERO (&rfds);
  FD_SET (fd, &rfds);
  tv.tv_sec = 0; tv.tv_usec = delay_ms * 1000;
  retval = select (fd+1, &rfds, NULL, NULL, &tv);
  return retval == 1 && retval != -1;
}


const char *terminal_response(void)
{
  static char buf[BUFSIZ * 2];
  int len = 0;
  fflush (stdout);

  while (has_data (tty_fd, 200) && len < BUFSIZ - 2)
  {
    if (read (tty_fd, &buf[len++], (size_t)1) == 1)
    {
       //fprintf (stderr, "uh\n");
    }
  }
  buf[--len] = 0;
  //close (tty);
  return buf;
}

int atty_readconfig (void)
{
#if 1
  if (_nc_raw ())
  {
    fprintf (stdout, "nc raw failed\n");
  }
#endif

  const char *cmd = "\033_Aa=q;\e\\";
  write (tty_fd, cmd, strlen (cmd));
  const char *ret = terminal_response ();
  if (ret[0])
  {
    if (!(ret[0] == '\e' &&
          ret[1] == '_' &&
          ret[2] == 'A'))
    {
     fprintf (stderr, "failed to initialize audio, unexpected response %li\n", strlen (ret));
     fflush (NULL);
     exit (-1);
    }
    if (strstr (ret, "s="))
    {
      sample_rate = atoi (strstr (ret, "s=")+2);
    }
    if (strstr (ret, "c="))
    {
      channels = atoi (strstr (ret, "c=")+2);
    }
    if (strstr (ret, "b="))
    {
      bits = atoi (strstr (ret, "b=")+2);
    }
    if (strstr (ret, "T="))
    {
      type = strstr (ret, "T=")[2];
    }
    if (strstr (ret, "e="))
    {
      encoding = strstr (ret, "e=")[2];
    }
    if (strstr (ret, "o="))
    {
      compression = strstr (ret, "o=")[2];
    }
  }
  else
  {
     fflush (NULL);
     _nc_noraw();
     return 0;
  }
  fflush (NULL);
  _nc_noraw();
  return 1;
}

void atty_status (void)
{
  _nc_noraw ();
  fprintf (stdout, "samplerate=%i ", sample_rate);
  fprintf (stdout, "channels=%i ", channels);
  fprintf (stdout, "bits=%i ", bits);

  switch (type)
  {
     case 'u':
        fprintf (stdout, "type=ulaw ");
        break;
     case 's':
        fprintf (stdout, "type=signed ");
        break;
     case 'f':
        fprintf (stdout, "type=float ");
        break;
     default:
        fprintf (stdout, "type=%c ", type);
        break;
  }

  switch (encoding)
  {
    default:
    case '0':
        fprintf (stdout, "encoding=none ");
        break;
    case 'b':
        fprintf (stdout, "encoding=base64 ");
        break;
    case 'a':
        fprintf (stdout, "encoding=ascii85 ");
        break;
  }
  switch (compression)
  {
    default:
        fprintf (stdout, "compression=none");
        break;
    case 'z':
        fprintf (stdout, "compression=z");
        break;
    case 'o':
        fprintf (stdout, "compression=opus");
        break;
  }
  fprintf (stdout, "\n");
  fflush (NULL);
}

void atty_mic (void);
void atty_speaker (void)
{
  uint8_t audio_packet[4096 * 4];
  uint8_t audio_packet_z[4096 * 4];
  uint8_t audio_packet_a85[4096 * 8];
  uint8_t *data = NULL;
  int  len = 0;

  signal (SIGINT, signal_int_speaker);
  signal (SIGTERM, signal_int_speaker);
  atexit (at_exit_speaker);

  lost_start = atty_ticks ();

  while (fread (buf, 1, 1, stdin) == 1)
  {
    audio_packet[len++]=buf[0];

    lost_end = atty_ticks();
    lost_time = (lost_end - lost_start);
    buffered_samples -= (sample_rate * lost_time / 1000);
    if (buffered_samples < 0)
      buffered_samples = 0;

    if (len >  buffer_samples)
    {
      uLongf encoded_len = sizeof (audio_packet_z);
      data = audio_packet;

      if (compression == 'z')
      {
        int z_result = compress (audio_packet_z, &encoded_len, data, encoded_len);
        if (z_result != Z_OK)
        {
          printf ("\e_Ao=z;zlib error\e\\");
          continue;
        }
        else
        {
          data = audio_packet_z;
        }
      }

      if (encoding == 'a')
      {
        int new_len = vt_a85enc (data, (char*)audio_packet_a85, encoded_len);
        audio_packet_a85[new_len]=0;
        data = audio_packet_a85;
      }
      if (encoding == 'b')
      {
        // TODO
      }

      fprintf (stdout, "\033_Af=%i;", len / channels / (bits/8));
      fwrite (data, 1, strlen ((char*)data), stdout);
      fwrite ("\e\\", 1, 2, stdout);
      fflush (stdout);
      usleep (1000 * ( len * 1000 / sample_rate - (atty_ticks()-lost_end)) );
      len = 0;
    }
    lost_start = atty_ticks ();
  }
}


int main (int argc, char **argv)
{
  char path[512];
  sprintf (path, "/proc/%d/fd/1", getppid());
  tty_fd = open (path, O_RDWR);

  char config[512]="";

  for (int i = 1; argv[i]; i++)
  {
    if (strchr (argv[i], '='))
    {
      const char *value = strchr (argv[i], '=')+1;
      char *key = strdup (argv[i]);
      *strchr(key, '=') = 0;
      if (!strcmp (key, "samplerate") || !strcmp (key, "s"))
      {
        sprintf (&config[strlen(config)],
                 "%ss=%i", config[0]?",":"", atoi(value));
      }
      else if (!strcmp (key, "bits") ||  !strcmp (key, "b"))
      {
        sprintf (&config[strlen(config)],
                 "%sb=%i", config[0]?",":"", atoi(value));
      }
      else if (!strcmp (key, "channels") ||  !strcmp (key, "c"))
      {
        sprintf (&config[strlen(config)],
                 "%sc=%i", config[0]?",":"", atoi(value));
      }
      else if (!strcmp (key, "type") ||  !strcmp (key, "T"))
      {
        if (!strcmp (value, "u")  ||
            !strcmp (value, "ulaw"))
        {
          sprintf (&config[strlen(config)], "%sT=u", config[0]?",":"");
        }
        else if (!strcmp (value, "s")  ||
                 !strcmp (value, "signed"))
        {
          sprintf (&config[strlen(config)], "%sT=s", config[0]?",":"");
        }
        else if (!strcmp (value, "f")  ||
                 !strcmp (value, "float"))
        {
          sprintf (&config[strlen(config)], "%sT=f", config[0]?",":"");
        }
      }
      else if (!strcmp (key, "encoding") || !strcmp (key, "e"))
      {
        if (!strcmp (value, "a")  ||
            !strcmp (value, "ascii85"))
        {
          sprintf (&config[strlen(config)], "%se=a", config[0]?",":"");
        }
        else if (!strcmp (value, "b")  ||
                 !strcmp (value, "base64"))
        {
          sprintf (&config[strlen(config)], "%se=b", config[0]?",":"");
        }
        else
        {
          sprintf (&config[strlen(config)], "%se=0", config[0]?",":"");
        }
      }
      else if (!strcmp (key, "compression") || !strcmp (key, "o"))
      {
        if (!strcmp (value, "opus")||
            !strcmp (value, "o"))
        {
          sprintf (&config[strlen(config)], "%so=o", config[0]?",":"");
        }
        else if (!strcmp (value, "deflate")||
                 !strcmp (value, "z"))
        {
          sprintf (&config[strlen(config)], "%so=z", config[0]?",":"");
        }
        else
        {
          sprintf (&config[strlen(config)], "%so=0", config[0]?",":"");
        }
      }
      free (key);
    }
    else
    {
      if (!strcmp (argv[i], "status"))
      {
        action = ACTION_STATUS;
      }
      else if (!strcmp (argv[i], "reset"))
      {
        action = ACTION_RESET;
      }
      else if (!strcmp (argv[i], "mic"))
      {
        action = ACTION_MIC;
      }
      else if (!strcmp (argv[i], "engine"))
      {
        action = ACTION_ENGINE;
      }
      else if (!strcmp (argv[i], "speaker"))
      {
        action = ACTION_SPEAKER;
      }
      else if (!strcmp (argv[i], "--help"))
      {
        _nc_noraw();
        printf ("Usage: atty [mic|speaker] key1=value key2=value\n");
        printf ("\n");
        printf ("Run atty alone to activate - or show status\n");
        return 0;
      }
    }
  }

  if (config[0])
  {
    printf ("\033_A%s;\e\\", config);
    fflush (NULL);
  }

  switch (action)
  {
    case ACTION_RESET:
      printf ("\033_As=8000,T=u,b=8,c=1,o=0,e=a;\e\\");
      fflush (NULL);
      /*  fallthrough */
    case ACTION_STATUS:

      if (atty_readconfig () == 0)
      {
         return atty_engine ();
      }
      atty_status ();
      break;
    case ACTION_SPEAKER:
      atty_readconfig ();
      atty_speaker ();
      break;
    case ACTION_MIC:
      atty_readconfig ();
      atty_mic ();
      break;
  }

  return 0;
}


static char a85_alphabet[]=
{
'!','"','#','$','%','&','\'','(',')','*',
'+',',','-','.','/','0','1','2','3','4',
'5','6','7','8','9',':',';','<','=','>',
'?','@','A','B','C','D','E','F','G','H',
'I','J','K','L','M','N','O','P','Q','R',
'S','T','U','V','W','X','Y','Z','[','\\',
']','^','_','`','a','b','c','d','e','f',
'g','h','i','j','k','l','m','n','o','p',
'q','r','s','t','u'
};

static char a85_decoder[256]="";

int vt_a85enc (const void *srcp, char *dst, int count)
{
  const uint8_t *src = srcp;
  int out_len = 0;
  int padding = 4 - (count % 4);
  for (int i = 0; i < (count+3)/4; i ++)
  {
    uint32_t input = 0;
    for (int j = 0; j < 4; j++)
    {
      input = (input << 8);
      if (i*4+j<count)
        input += src[i*4+j];
    }

    int divisor = 85 * 85 * 85 * 85;
    if (input == 0)
    {
        dst[out_len++] = 'z';
    }
    else
    {
      for (int j = 0; j < 5; j++)
      {
        dst[out_len++] = a85_alphabet[(input / divisor) % 85];
        divisor /= 85;
      }
    }
  }
  out_len -= padding;
  dst[out_len++]='~';
  dst[out_len++]='>';
  dst[out_len]=0;
  return out_len;
}

int vt_a85dec (const char *src, char *dst, int count)
{
  if (a85_decoder[0] == 0)
  {
    for (int i = 0; i < 85; i++)
    {
      a85_decoder[(int)a85_alphabet[i]]=i;
    }
  }
  int out_len = 0;
  uint32_t val = 0;
  int k = 0;

  for (int i = 0; i < count; i ++, k++)
  {
    val *= 85;

    if (src[i] == '~')
      break;
    else if (src[i] == 'z')
    {
      for (int j = 0; j < 4; j++)
        dst[out_len++] = 0;
      k = 0;
    }
    else
    {
      val += a85_decoder[(int)src[i]];
      if (k % 5 == 4)
      {
         for (int j = 0; j < 4; j++)
         {
           dst[out_len++] = (val & (0xff << 24)) >> 24;
           val <<= 8;
         }
         val = 0;
      }
    }
  }
  k = k % 5;
  if (k)
  {
    for (int j = k; j < 4; j++)
    {
      val += 84;
      val *= 85;
    }

    for (int j = 0; j < 4; j++)
    {
      dst[out_len++] = (val & (0xff << 24)) >> 24;
      val <<= 8;
    }
    val = 0;
    out_len -= (5-k);
  }
  dst[out_len]=0;
  return out_len;
}

int vt_a85len (const char *src, int count)
{
  int out_len = 0;
  int k = 0;

  for (int i = 0; src[i] && i < count; i ++, k++)
  {
    if (src[i] == '~')
      break;
    else if (src[i] == 'z')
    {
      out_len += 4;
      k = 0;
    }
    else
    {
      if (k % 5 == 4)
      {
        out_len += 4;
      }
    }
  }
  k = k % 5;
  if (k)
  {
    out_len += 4;
    out_len -= (5-k);
  }
  return out_len;
}


#define MIN(a,b) ((a)<(b)?(a):(b))

static int in_audio_data = 0;

static char audio_packet[65536];
static int audio_packet_pos = 0;

static int iterate (int timeoutms)
{
  unsigned char buf[20];
  int length;

  {
    int elapsed = 0;
    int got_event = 0;

    do {
#define DELAY_MS 100
      if (!got_event)
        got_event = has_data (STDIN_FILENO, MIN(DELAY_MS, timeoutms-elapsed));
      elapsed += MIN(DELAY_MS, timeoutms-elapsed);
      if (!got_event && timeoutms && elapsed >= timeoutms)
        return 1;
    } while (!got_event);
  }

  if (in_audio_data)
  {
    while (read (STDIN_FILENO, &buf[0], 1) != -1)
    {
      if (buf[0] == '\e')
      {
        in_audio_data = 2;
      }
      else if (buf[0] == '\\' &&
               in_audio_data == 2)
      {
        if (encoding == 'a')
        {
          char *temp = malloc (vt_a85len (audio_packet, audio_packet_pos));
          int len = vt_a85dec (audio_packet, temp, audio_packet_pos);
          fwrite (temp, 1, len, stdout);
          fflush (stdout);
          free (temp);
        }
        else if (encoding == 'b')
        {
          uint8_t *temp = malloc (audio_packet_pos);
          int len = audio_packet_pos;
          vt_base642bin (audio_packet,
                  &len,
                  temp);
          fwrite (temp, 1, len, stdout);
          fflush (stdout);
          free (temp);
        }

        audio_packet_pos = 0;
        in_audio_data = 0;
        return 1;
      }
      else
      {
        in_audio_data = 1;
        audio_packet[audio_packet_pos++] = buf[0];
        if (audio_packet_pos > 65535) audio_packet_pos = 65535;
      }
    }
    return 1;
  }

  for (length = 0; length < 10; length ++)
    if (read (STDIN_FILENO, &buf[length], 1) != -1)
      {
         if (buf[0] == 3) /*  control-c */
         {
           return 0;
         }
         else if (!strncmp ((void*)buf, "\033_A", MIN(length+1,3)))
         {
           int semis = 0;
           while (semis < 1 && read (STDIN_FILENO, &buf[0], 1) != -1)
           {
             if (buf[0] == ';') semis ++;
           }
           in_audio_data = 1;
           return 1;
         }
      }
  return 1;
}

/*  return 0 if stopping */
int iterate (int timeoutms);

void atty_mic (void)
{
  signal(SIGINT,signal_int_mic);
  signal(SIGTERM,signal_int_mic);
  _nc_raw ();
  fprintf(stderr, "\033_Am=1;\e\\");
  fflush (NULL);
  while (iterate (1000));
  at_exit_mic ();
}

static const char *base64_map="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
static void bin2base64_group (const unsigned char *in, int remaining, char *out)
{
  unsigned char digit[4] = {0,0,64,64};
  int i;
  digit[0] = in[0] >> 2;
  digit[1] = ((in[0] & 0x03) << 4) | ((in[1] & 0xf0) >> 4);
  if (remaining > 1)
    {
      digit[2] = ((in[1] & 0x0f) << 2) | ((in[2] & 0xc0) >> 6);
      if (remaining > 2)
        digit[3] = ((in[2] & 0x3f));
    }
  for (i = 0; i < 4; i++)
    out[i] = base64_map[digit[i]];
}

void
vt_bin2base64 (const void *bin,
               int         bin_length,
               char       *ascii)
{
  /* this allocation is a hack to ensure we always produce the same result,
   * regardless of padding data accidentally taken into account.
   */
  unsigned char *bin2 = calloc (bin_length + 4, 1);
  unsigned const char *p = bin2;
  int i;
  memcpy (bin2, bin, bin_length);
  for (i=0; i*3 < bin_length; i++)
   {
     int remaining = bin_length - i*3;
     bin2base64_group (&p[i*3], remaining, &ascii[i*4]);
   }
  free (bin2);
  ascii[i*4]=0;
}

static unsigned char base64_revmap[255];
static void base64_revmap_init (void)
{
  static int done = 0;
  if (done)
    return;

  for (int i = 0; i < 255; i ++)
    base64_revmap[i]=255;
  for (int i = 0; i < 64; i ++)
    base64_revmap[((const unsigned char*)base64_map)[i]]=i;
  /* include variants used in URI encodings for decoder,
   * even if that is not how we encode
  */
  base64_revmap['-']=62;
  base64_revmap['_']=63;
  base64_revmap['+']=62;
  base64_revmap['/']=63;

  done = 1;
}

int
vt_base642bin (const char    *ascii,
               int           *length,
               unsigned char *bin)
{
  int i;
  int charno = 0;
  int outputno = 0;
  int carry = 0;
  base64_revmap_init ();
  for (i = 0; ascii[i]; i++)
    {
      int bits = base64_revmap[((const unsigned char*)ascii)[i]];
      if (length && outputno > *length)
        {
          *length = -1;
          return -1;
        }
      if (bits != 255)
        {
          switch (charno % 4)
            {
              case 0:
                carry = bits;
                break;
              case 1:
                bin[outputno] = (carry << 2) | (bits >> 4);
                outputno++;
                carry = bits & 15;
                break;
              case 2:
                bin[outputno] = (carry << 4) | (bits >> 2);
                outputno++;
                carry = bits & 3;
                break;
              case 3:
                bin[outputno] = (carry << 6) | bits;
                outputno++;
                carry = 0;
                break;
            }
          charno++;
        }
    }
  bin[outputno]=0;
  if (length)
    *length= outputno;
  return outputno;
}


////

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

int   do_quit      = 0;

static pid_t vt_child;
static MrgVT *vt = NULL;

void
signal_child (int signum)
{
  pid_t pid;
  int   status;
  while ((pid = waitpid(-1, &status, WNOHANG)) != -1)
    {
      if (pid)
      {
      if (pid == vt_child)
      {
        _nc_noraw ();
        printf ("\n");
        exit(0);
        do_quit = 1;
        return;
      }
      else
      {
        fprintf (stderr, "child signal ? %i %i\n", pid, vt_child);
      }
    }
    }
}


const char *ctx_vt_find_shell_command (void)
{
  int i;
  const char *command = NULL;
  struct stat stat_buf;
  static char *alts[][2] ={
    {"/bin/bash",     "/bin/bash -i"},
    {"/usr/bin/bash", "/usr/bin/bash -i"},
    {"/bin/sh",       "/bin/sh -i"},
    {"/usr/bin/sh",   "/usr/bin/sh -i"},
    {NULL, NULL}
  };
  for (i = 0; alts[i][0] && !command; i++)
  {
    lstat (alts[i][0], &stat_buf);
    if (S_ISREG(stat_buf.st_mode) || S_ISLNK(stat_buf.st_mode))
      command = alts[i][1];
  }
  return command;
}

static void vt_state_neutral      (MrgVT *vt, int byte);
static void vt_state_esc          (MrgVT *vt, int byte);
static void vt_state_osc          (MrgVT *vt, int byte);
static void vt_state_apc          (MrgVT *vt, int byte);
static void vt_state_apc_generic  (MrgVT *vt, int byte);
static void vt_state_esc_sequence (MrgVT *vt, int byte);
static void vt_state_esc_foo      (MrgVT *vt, int byte);
static void vt_state_swallow      (MrgVT *vt, int byte);

void vtpty_resize (void *data, int cols, int rows, int px_width, int px_height)
{
  VtPty *vtpty = data;
  struct winsize ws;

  ws.ws_row = rows;
  ws.ws_col = cols;
  ws.ws_xpixel = px_width;
  ws.ws_ypixel = px_height;
  ioctl(vtpty->pty, TIOCSWINSZ, &ws);
}

static ssize_t vtpty_write (void *data, const void *buf, size_t count)
{
  VtPty *vtpty = data;
  return write (vtpty->pty, buf, count);
}

static ssize_t vtpty_read (void  *data, void *buf, size_t count)
{
  VtPty *vtpty = data;
  return read (vtpty->pty, buf, count);
}

static int vtpty_waitdata (void  *data, int timeout)
{
  VtPty *vtpty = data;
  struct timeval tv;
  fd_set fdset;
  FD_ZERO (&fdset);
  FD_SET(vtpty->pty, &fdset);

  tv.tv_sec = 0;
  tv.tv_usec = timeout;
  tv.tv_sec  = timeout / 1000000;
  tv.tv_usec = timeout % 1000000;

  if (select (vtpty->pty+1, &fdset, NULL, NULL, &tv) == -1){
    perror("select");
    return 0;
  }
  if (FD_ISSET(vtpty->pty, &fdset))
  {
    return 1;
  }
  return 0;
}

static void vt_resize (int sig)
{
  struct winsize ws;
  if (!vt->vtpty.pty)
    return;
  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
    return;
  vtpty_resize (&vt->vtpty, ws.ws_row, ws.ws_col, ws.ws_xpixel, ws.ws_ypixel);
}

static void ctx_vt_run_command (MrgVT *vt, const char *command)
{
  struct winsize ws;

  static int reaper_started = 0;
  if (!reaper_started)
  {
    reaper_started = 1;
    signal (SIGCHLD, signal_child);
  }

  ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws);
  signal (SIGWINCH, vt_resize);

  vt->vtpty.pid = forkpty (&vt->vtpty.pty, NULL, NULL, &ws);
  if (vt->vtpty.pid == 0)
  {
    system (command);
    exit(0);
  }
  else if (vt->vtpty.pid < 0)
  {
    fprintf (stderr, "forkpty failed\n");
  }
  //fcntl(vt->vtpty.pty, F_SETFL, O_NONBLOCK);
}

static void vtcmd_reset_to_initial_state (MrgVT *vt, const char *sequence)
{
  vt->audio.bits = 8;
  vt->audio.channels = 1;
  vt->audio.type = 'u';
  vt->audio.samplerate = 8000;
  vt->audio.encoding = 'a';
  vt->audio.compression = '0';
  vt->audio.mic = 0;
}


MrgVT *ctx_vt_new (const char *command, int cols, int rows, float font_size, float line_spacing)
{
  MrgVT *vt         = calloc (sizeof (MrgVT), 1);
  vt->state         = vt_state_neutral;
  vt->waitdata      = vtpty_waitdata;
  vt->read          = vtpty_read;
  vt->write         = vtpty_write;
  vt->resize        = vtpty_resize;
  vt->bell = 4;

  vt->argument_buf_len   = 0;
  vt->argument_buf_cap   = 64;
  vt->argument_buf       = malloc (vt->argument_buf_cap);
  vt->argument_buf[0]    = 0;

  if (command)
  {
    ctx_vt_run_command (vt, command);
  }

  if (cols <= 0) cols = 80;
  if (rows <= 0) cols = 24;

  //ctx_vt_set_term_size (vt, cols, rows);

  vtcmd_reset_to_initial_state (vt, NULL);

  return vt;
}

int ctx_vt_poll (MrgVT *vt, int timeout)
{
  int read_size = sizeof(buf);
  int got_data = 0;
  int remaining_chars = 1024 * 1024;
  int len = 0;
    audio_task (vt, 0);


  while (has_data (STDIN_FILENO, 10))
  {
    uint8_t c;
    read (STDIN_FILENO, &c, (size_t)1);
    vt_write (vt, &c, 1);
  }

  read_size = MIN(read_size, remaining_chars);
  while (timeout > 100 &&
         remaining_chars > 0 &&
         vt_waitdata (vt, timeout))
  {
    len = vt_read (vt, buf, read_size);
    for (int i = 0; i < len; i++)
      vt->state (vt, buf[i]);
    got_data+=len;
    remaining_chars -= len;
    timeout -= 10;
    audio_task (vt, 0);
  }
  fflush (NULL);
  return got_data;
}

void ctx_vt_destroy (MrgVT *vt)
{
  free (vt->argument_buf);

  kill (vt->vtpty.pid, 9);
  close (vt->vtpty.pty);
  free (vt);
}

int atty_engine (void)
{
  const char *shell = NULL;
  printf ("atty v0.0\n");
  _nc_raw ();
  setsid();
  vt = ctx_vt_new (shell?shell:ctx_vt_find_shell_command(), 80, 24, 14, 1.0);

  int sleep_time = 2500;

  vt_child = vt->vtpty.pid;
  signal (SIGCHLD, signal_child);
  ctx_vt_bell (vt);
  while(!do_quit)
  {
    ctx_vt_poll (vt, sleep_time);
  }
  ctx_vt_destroy (vt);
  _nc_noraw ();
  return 0;
}

static void ctx_vt_argument_buf_reset (MrgVT *vt, const char *start)
{
  if (start)
  {
    strcpy (vt->argument_buf, start);
    vt->argument_buf_len = strlen (start);
  }
  else
    vt->argument_buf[vt->argument_buf_len=0]=0;
}

static inline void ctx_vt_argument_buf_add (MrgVT *vt, int ch)
{
  if (vt->argument_buf_len + 1 >= 1024 * 1024 * 2)
    return; // XXX : perhaps we should bail at 1mb + 1kb ?
            //       
  if (vt->argument_buf_len + 1 >=
      vt->argument_buf_cap)
  {
    vt->argument_buf_cap = vt->argument_buf_cap * 1.5;
    vt->argument_buf = realloc (vt->argument_buf, vt->argument_buf_cap);
  }

  vt->argument_buf[vt->argument_buf_len] = ch;
  vt->argument_buf[++vt->argument_buf_len] = 0;
}

static void vt_state_swallow (MrgVT *vt, int byte)
{
  vt->state = vt_state_neutral;
}
static void ensure_title (MrgVT *vt);

static void vt_state_apc_audio (MrgVT *vt, int byte)
{
  if ((byte < 32) && ( (byte < 8) || (byte > 13)) )
  {
    vt_audio (vt, vt->argument_buf);
    ensure_title (vt);
    vt->state = ((byte == 27) ?  vt_state_swallow : vt_state_neutral);
  }
  else
  {
    ctx_vt_argument_buf_add (vt, byte);
  }
}


static void vt_state_apc_generic (MrgVT *vt, int byte)
{
  if ((byte < 32) && ((byte < 8) || (byte > 13)))
  {
    vt->state = ((byte == 27) ?  vt_state_swallow : vt_state_neutral);
  }
  else
  {
    ctx_vt_argument_buf_add (vt, byte);
  }
}

static void vt_state_apc (MrgVT *vt, int byte)
{
  if (byte == 'A')
  {
    ctx_vt_argument_buf_add (vt, byte);
    vt->state = vt_state_apc_audio;
  }
  else if ((byte < 32) && ( (byte < 8) || (byte > 13)) )
  {
    vt->state = ((byte == 27) ?  vt_state_swallow : vt_state_neutral);
  }
  else
  {
    ctx_vt_argument_buf_add (vt, byte);
    vt->state = vt_state_apc_generic;
  }
}

static void handle_sequence (MrgVT *vt, const char *sequence)
{
  printf ("\e%s", sequence);
}

static void vt_state_esc_foo (MrgVT *vt, int byte)
{
  ctx_vt_argument_buf_add (vt, byte);
  vt->state = vt_state_neutral;
  handle_sequence (vt, vt->argument_buf);
}

static void vt_state_esc_sequence (MrgVT *vt, int byte)
{
  if (byte < ' ' && byte != 27)
  {
    printf ("%c", byte);
  }
  else
  {
    if (byte == 27)
    {
    }
    else if (byte >= '@' && byte <= '~')
    {
      ctx_vt_argument_buf_add (vt, byte);
      vt->state = vt_state_neutral;
      handle_sequence (vt, vt->argument_buf);
    }
    else
    {
      ctx_vt_argument_buf_add (vt, byte);
    }
  }
}

static inline int parse_int (const char *arg, int def_val)
{
  if (!isdigit (arg[1]) || strlen (arg) == 2)
    return def_val;
  return atoi (arg+1);
}

static int last_title_mic = 0;

static void update_title (MrgVT *vt)
{
  if (vt->audio.mic)
  {
    printf ("\e]0;mic|");
  }
  else
  {
    printf ("\e]0;atty|");
  }
  printf ("%s\e\\", vt->title?vt->title:"");
  last_title_mic = vt->audio.mic;
}

static void ensure_title (MrgVT *vt)
{
  if (vt->audio.mic != last_title_mic)
  {
    update_title (vt);
  }
}

static void vt_state_osc (MrgVT *vt, int byte)
{
      if ((byte < 32) && ( (byte < 8) || (byte > 13)) )
      {
          int n = parse_int (vt->argument_buf, 0);
          switch (n)
          {
          case 0:
            if (vt->title) free (vt->title);
            vt->title = strdup (vt->argument_buf + 3);

            if (byte == 27)
              vt->state = vt_state_swallow;
            else
              vt->state = vt_state_neutral;
            update_title (vt);
            return;
          default:
            printf ("\e%s", vt->argument_buf);
            break;
          }
        if (byte == 27)
        {
          vt->state = vt_state_swallow;
          printf ("\e\\");
        }
        else
        {
          vt->state = vt_state_neutral;
          printf ("%c", byte);
        }
      }
      else
      {
        ctx_vt_argument_buf_add (vt, byte);
      }
}

static void vt_state_esc (MrgVT *vt, int byte)
{
  if (byte < ' ' && byte != 27)
  {
    printf ("%c", byte);
  }
  else
  switch (byte)
  {
    case 27: /* ESCape */
            printf ("%c", byte);
            break;
    case ')':
    case '#':
    case '(':
      {
        char tmp[]={byte, '\0'};
        ctx_vt_argument_buf_reset(vt, tmp);
        vt->state = vt_state_esc_foo;
      }
      break;
    case '[':
    case '%':
    case '+':
    case '*':
      {
        char tmp[]={byte, '\0'};
        ctx_vt_argument_buf_reset(vt, tmp);
        vt->state = vt_state_esc_sequence;
      }
      break;
    case ']':
      {
        char tmp[]={byte, '\0'};
        ctx_vt_argument_buf_reset(vt, tmp);
        vt->state = vt_state_osc;
      }
      break;
    case '^':  // privacy message 
    case '_':  // APC
      {
        char tmp[]={byte, '\0'};
        ctx_vt_argument_buf_reset(vt, tmp);
        vt->state = vt_state_apc;
      }
      break;
    default:
      {
        char tmp[]={byte, '\0'};
        tmp[0]=byte;
        vt->state = vt_state_neutral;
        handle_sequence (vt, tmp);
      }
      break;
  }
}

static void vt_state_neutral (MrgVT *vt, int byte)
{
  if (byte < ' ' && byte != 27)
  {
    printf ("%c", byte);
  }
  else
  switch (byte)
  {
    case 27: /* ESCape */
      vt->state = vt_state_esc;
      break;
    default:
      printf ("%c", byte);
      break;
  }
}
