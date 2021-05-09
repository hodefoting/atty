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
#include <zlib.h>

#include "a85.h"
#include "base64.h"

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

static struct termios orig_attr; /* in order to restore at exit */
static int    nc_is_raw = 0;
int atty_vt (int argc, char **argv);

int tty_fd = STDIN_FILENO;
static unsigned char buf[BUFSIZ];

void atty_noraw (void)
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
  atty_noraw();
  fflush (NULL);
  usleep (1000 * 100);
}

static void
at_exit_speaker (void)
{
  atty_noraw();
  fflush (NULL);
}

int atty_raw (void)
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

int buffered_bytes = 0;
int sample_rate = 8000;
int bits = 8;
int buffer_size = 512;
int channels = 1;
int ulaw = 1;
int compression = '0';
int encoding = '0';
int type = 'u';
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

  while (has_data (tty_fd, 500) && len < BUFSIZ - 2)
  {
    read (tty_fd, &buf[len++], (size_t)1);
  }
  if (len > 0)
  buf[--len] = 0;
  return buf;
}

int atty_readconfig (void)
{
#if 1
  if (atty_raw ())
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
    //fprintf (stderr, "[%s]\n", ret+2);
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
    if (strstr (ret, "B="))
    {
      buffer_size = atoi (strstr (ret, "B=")+2);
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
     atty_noraw();
     return 0;
  }
  fflush (NULL);
  atty_noraw();
  return 1;
}

void atty_status (void)
{
  atty_noraw ();
  fprintf (stdout, "samplerate=%i\n", sample_rate);
  fprintf (stdout, "channels=%i\n", channels);
  fprintf (stdout, "bits=%i\n", bits);
  fprintf (stdout, "buffer_size=%i\n", buffer_size);

  switch (type)
  {
     case 'u':
        fprintf (stdout, "type=ulaw\n");
        break;
     case 's':
        fprintf (stdout, "type=signed\n");
        break;
     case 'f':
        fprintf (stdout, "type=float\n");
        break;
     default:
        fprintf (stdout, "type=%c\n", type);
        break;
  }

  switch (encoding)
  {
    default:
        fprintf (stdout, "encoding=%c\n", encoding);
        break;
    case '0':
        fprintf (stdout, "encoding=none\n");
        break;
    case 'b':
        fprintf (stdout, "encoding=base64\n");
        break;
    case 'a':
        fprintf (stdout, "encoding=ascii85\n");
        break;
  }
  switch (compression)
  {
    default:
        fprintf (stdout, "compression=none\n");
        break;
    case 'z':
        fprintf (stdout, "compression=z\n");
        break;
    case 'o':
        fprintf (stdout, "compression=opus\n");
        break;
  }
  fflush (NULL);
}

void atty_mic (void);
void atty_speaker (void);


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
      else if (!strcmp (key, "buffer_size") ||  !strcmp (key, "B"))
      {
        sprintf (&config[strlen(config)],
                 "%sB=%i", config[0]?",":"", atoi(value));
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
        atty_noraw();
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
         return atty_vt (argc, argv);
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

/////////

void atty_speaker (void)
{
  uint8_t audio_packet[4096 * 4];
  uint8_t audio_packet_z[4096 * 4];
  uint8_t audio_packet_a85[4096 * 8];
  uint8_t *data = NULL;
  int  len = 0;

  int byte_rate = sample_rate * bits/8 * channels;

  signal (SIGINT, signal_int_speaker);
  signal (SIGTERM, signal_int_speaker);
  atexit (at_exit_speaker);

  lost_start = atty_ticks ();

  while (fread (buf, 1, 1, stdin) == 1)
  {
    audio_packet[len++]=buf[0];

    lost_end = atty_ticks();
    lost_time += (lost_end - lost_start);
    buffered_bytes -= (byte_rate * lost_time / 1000);
    lost_time = 0;

    if (buffered_bytes < 0)
      buffered_bytes = 0;

    if (buffered_bytes > buffer_size * 2)
    {
      int wait_bytes = buffered_bytes - buffer_size * 2;
      usleep (wait_bytes * 1000 * 1000 / byte_rate);
      buffered_bytes = buffer_size * 2;
    }
    lost_start = atty_ticks ();

    if (len >= buffer_size)
    {
      uLongf encoded_len = sizeof (audio_packet_z);
      data = audio_packet;
      int data_len = encoded_len;

      if (compression == 'z')
      {
        int z_result = compress (audio_packet_z, &encoded_len,
                                 data, encoded_len);
        if (z_result != Z_OK)
        {
          printf ("\e_Ao=z;zlib error-\e\\");
          continue;
        }
        else
        {
          data = audio_packet_z;
        }
        data_len = encoded_len;
      }

      if (encoding == 'a')
      {
        int new_len = a85enc (data, (char*)audio_packet_a85, encoded_len);
        audio_packet_a85[new_len]=0;
        data = audio_packet_a85;
        data_len = new_len;
      }
      else if (encoding == 'b')
      {
        int new_len = ctx_bin2base64 (data, 
            encoded_len,
            (char*)audio_packet_a85);
        data = audio_packet_a85;
        data_len = new_len;
      }
      else
      {
        // we need a text encoding
        return;
      }

      fprintf (stdout, "\033_Af=%i;", len / channels / (bits/8));
      //fwrite (data, 1, strlen ((char*)data), stdout);
      fwrite (data, 1, data_len, stdout);
      fwrite ("\e\\", 1, 2, stdout);
      fflush (stdout);

      buffered_bytes += len;
      len = 0;
    }
  }
}

///////
// timestamped packets of audio is better..
// this would also permit 
///////

static int in_audio_data = 0;

static char audio_packet[65536];
static int audio_packet_pos = 0;
static int frames = 0;

static int mic_iterate (int timeoutms)
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
          unsigned char *temp = malloc (a85len (audio_packet, audio_packet_pos));
          int len = a85dec (audio_packet, (char*)temp, audio_packet_pos);

          if (compression == 'z')
          {
            unsigned long actual_uncompressed_size = frames * bits/8 * channels + 16;
            unsigned char *data2 = malloc (actual_uncompressed_size);
      /* if a buf size is set (rather compression, but
       * this works first..) then */
            int z_result = uncompress (data2, &actual_uncompressed_size,
                                 temp, len);
            if (z_result == Z_OK || z_result == Z_BUF_ERROR)
            {
              if (z_result != Z_OK)
                 fprintf (stderr, "[[z error:%i %i]]", __LINE__, z_result);
              fwrite (data2, 1, actual_uncompressed_size, stdout);
            }
            free (data2);
          }
          else
          {
            fwrite (temp, 1, len, stdout);
          }

          fflush (stdout);
          free (temp);
        }
        else if (encoding == 'b')
        {
          uint8_t *temp = malloc (audio_packet_pos);
          int len = audio_packet_pos;
          ctx_base642bin (audio_packet,
                  &len,
                  temp);
          // XXX : NYI compression inside base64

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
           char tmp[40];
           int tmpl=0;
           int semis = 0;
           frames = 0;

           while (semis < 1 && read (STDIN_FILENO, &buf[0], 1) != -1)
           {
             tmp[tmpl++]=buf[0];
             if (tmpl>=40)tmpl=39;
             tmp[tmpl]=0;
             if (buf[0] == ';') semis ++;
           }
           if (strstr ((char*)tmp, "f="))
           {
             frames = atoi (strstr ((char*)tmp, "f=")+2);
           }
           in_audio_data = 1;
           return 1;
         }
      }
  return 1;
}

void atty_mic (void)
{
  signal(SIGINT,signal_int_mic);
  signal(SIGTERM,signal_int_mic);
  atty_raw ();
  fprintf(stderr, "\033_Am=1;\e\\");
  fflush (NULL);
  while (mic_iterate (1000));
  at_exit_mic ();
}

