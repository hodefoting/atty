
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

#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <termios.h>
#include <pty.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#include "a85.h"
#include "base64.h"

int has_data (int fd, int delay_ms);
void atty_noraw (void);
void atty_raw (void);

static unsigned char buf[BUFSIZ];

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
  int compression; // z zlib o opus
  int buffer_size;

  int frames;

  uint8_t *data;
  int      data_size;
} AudioState;

typedef struct VtPty {
  int        pty;
  pid_t      pid;
} VtPty;

typedef struct _VT VT;

struct _VT {
  char     *title;
  void    (*state)(VT *vt, int byte);
  int       bell;
#define MAX_ARGUMENT_BUF_LEN (8192 + 16)

  char     *argument_buf;
  int       argument_buf_len;
  int       argument_buf_cap;
  ssize_t (*write)(void *serial_obj, const void *buf, size_t count);
  ssize_t (*read)(void *serial_obj, void *buf, size_t count);
  int     (*waitdata)(void *serial_obj, int timeout);
  void    (*resize)(void *serial_obj, int cols, int rows, int px_width, int px_height);

  VtPty      vtpty;

  AudioState audio;
};

static ssize_t vt_write (VT *vt, const void *buf, size_t count)
{
  if (!vt->write) return 0;
  return vt->write (&vt->vtpty, buf, count);
}
static ssize_t vt_read (VT *vt, void *buf, size_t count)
{
  if (!vt->read) return 0;
  return vt->read (&vt->vtpty, buf, count);
}
static int vt_waitdata (VT *vt, int timeout)
{
  if (!vt->waitdata) return 0;
  return vt->waitdata (&vt->vtpty, timeout);
}

/* NOTE : the reason the source is split the way it is, is that the
 *        audio-engine originates in another project
 */
#include "vt-audio.h"
int   do_quit      = 0;

static pid_t vt_child;
static VT *vt = NULL;

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
#if 0
        atty_noraw ();
#endif
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


const char *vt_find_shell_command (void)
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

static void vt_state_neutral      (VT *vt, int byte);
static void vt_state_esc          (VT *vt, int byte);
static void vt_state_osc          (VT *vt, int byte);
static void vt_state_apc          (VT *vt, int byte);
static void vt_state_apc_generic  (VT *vt, int byte);
static void vt_state_esc_sequence (VT *vt, int byte);
static void vt_state_esc_foo      (VT *vt, int byte);
static void vt_state_swallow      (VT *vt, int byte);

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
  vtpty_resize (&vt->vtpty, ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel);
}

static void vt_run_command (VT *vt, const char *command)
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

static void vtcmd_reset_to_initial_state (VT *vt, const char *sequence)
{
  vt->audio.bits        = 8;
  vt->audio.channels    = 1;
  vt->audio.type        = 'u';
  vt->audio.samplerate  = 8000;
  vt->audio.encoding    = 'a';
  vt->audio.compression = '0';
  vt->audio.mic         = 0;
}


VT *vt_new (const char *command, int cols, int rows, float font_size, float line_spacing)
{
  VT *vt         = calloc (sizeof (VT), 1);
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
    vt_run_command (vt, command);
  }

  if (cols <= 0) cols = 80;
  if (rows <= 0) cols = 24;

  //vt_set_term_size (vt, cols, rows);

  vtcmd_reset_to_initial_state (vt, NULL);

  return vt;
}

int vt_poll (VT *vt, int timeout)
{
  int read_size = sizeof(buf);
  int got_data = 0;
  int remaining_chars = 1024 * 1024;
  int len = 0;
  vt_audio_task (vt, 0);

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
    vt_audio_task (vt, 0);
  }
  fflush (NULL);
  return got_data;
}

void vt_destroy (VT *vt)
{
  free (vt->argument_buf);

  kill (vt->vtpty.pid, 9);
  close (vt->vtpty.pty);
  free (vt);
}

int atty_vt (int argc, char **argv)
{
  const char *shell = NULL;
  printf ("atty v0.0\n");
  atty_raw ();
  setsid();
  vt = vt_new (shell?shell:vt_find_shell_command(), 80, 24, 14, 1.0);

  int sleep_time = 2500;

  vt_child = vt->vtpty.pid;
  signal (SIGCHLD, signal_child);
  vt_bell (vt);
  while(!do_quit)
  {
    vt_poll (vt, sleep_time);
  }
  vt_destroy (vt);
  atty_noraw ();
  return 0;
}

static void vt_argument_buf_reset (VT *vt, const char *start)
{
  if (start)
  {
    strcpy (vt->argument_buf, start);
    vt->argument_buf_len = strlen (start);
  }
  else
    vt->argument_buf[vt->argument_buf_len=0]=0;
}

static inline void vt_argument_buf_add (VT *vt, int ch)
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

static void vt_state_swallow (VT *vt, int byte)
{
  vt->state = vt_state_neutral;
}
static void ensure_title (VT *vt);

static void vt_state_apc_audio (VT *vt, int byte)
{
  if ((byte < 32) && ( (byte < 8) || (byte > 13)) )
  {
    vt_audio (vt, vt->argument_buf);
    ensure_title (vt);
    vt->state = ((byte == 27) ?  vt_state_swallow : vt_state_neutral);
  }
  else
  {
    vt_argument_buf_add (vt, byte);
  }
}


static void vt_state_apc_generic (VT *vt, int byte)
{
  if ((byte < 32) && ((byte < 8) || (byte > 13)))
  {
    vt->state = ((byte == 27) ?  vt_state_swallow : vt_state_neutral);
  }
  else
  {
    vt_argument_buf_add (vt, byte);
  }
}

static void vt_state_apc (VT *vt, int byte)
{
  if (byte == 'A')
  {
    vt_argument_buf_add (vt, byte);
    vt->state = vt_state_apc_audio;
  }
  else if ((byte < 32) && ( (byte < 8) || (byte > 13)) )
  {
    vt->state = ((byte == 27) ?  vt_state_swallow : vt_state_neutral);
  }
  else
  {
    vt_argument_buf_add (vt, byte);
    vt->state = vt_state_apc_generic;
  }
}

static void handle_sequence (VT *vt, const char *sequence)
{
  printf ("\e%s", sequence);
}

static void vt_state_esc_foo (VT *vt, int byte)
{
  vt_argument_buf_add (vt, byte);
  vt->state = vt_state_neutral;
  handle_sequence (vt, vt->argument_buf);
}

static void vt_state_esc_sequence (VT *vt, int byte)
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
      vt_argument_buf_add (vt, byte);
      vt->state = vt_state_neutral;
      handle_sequence (vt, vt->argument_buf);
    }
    else
    {
      vt_argument_buf_add (vt, byte);
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

static void update_title (VT *vt)
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

static void ensure_title (VT *vt)
{
  if (vt->audio.mic != last_title_mic)
  {
    update_title (vt);
  }
}

static void vt_state_osc (VT *vt, int byte)
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
        vt_argument_buf_add (vt, byte);
      }
}

static void vt_state_esc (VT *vt, int byte)
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
        vt_argument_buf_reset(vt, tmp);
        vt->state = vt_state_esc_foo;
      }
      break;
    case '[':
    case '%':
    case '+':
    case '*':
      {
        char tmp[]={byte, '\0'};
        vt_argument_buf_reset(vt, tmp);
        vt->state = vt_state_esc_sequence;
      }
      break;
    case ']':
      {
        char tmp[]={byte, '\0'};
        vt_argument_buf_reset(vt, tmp);
        vt->state = vt_state_osc;
      }
      break;
    case '^':  // privacy message 
    case '_':  // APC
      {
        char tmp[]={byte, '\0'};
        vt_argument_buf_reset(vt, tmp);
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

static void vt_state_neutral (VT *vt, int byte)
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
