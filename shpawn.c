#define _XOPEN_SOURCE

#include <stddef.h> // NULL
#include <stdio.h> // perror, fprintf, stderr, sprintf, puts, printf
#include <signal.h> // kill, SIGCONT
#include <stdlib.h> // exit, ptsname, grantpt, unlockpt, size_t, ssize_t
#include <unistd.h> // close, write, read, execv, dup2, fork
#include <string.h> // strlen
#include <stdint.h> // uint8_t

// open, O_WRONLY, O_RDONLY, O_RDWR:
#include <sys/stat.h>
#include <fcntl.h> // fcntl, F_GETFL, F_SETFL, O_NONBLOCK

#include <linux/limits.h> // PATH_MAX, NAME_MAX


signed char _shstat(int pid)
{
  int fd = -1;
  char path[PATH_MAX];
  // "%pid% (%cmd%) %state%\0"
  // pid - maximum 20 characters (if 64-bit unsigned)
  // cmd - maximum NAME_MAX characters
  // state - 1 character
  //                      p   _   (   c   )   _   s
  const int STATSTR_min = 1 + 1 + 1 + 1 + 1 + 1 + 1;
  //                      p    _   (       c      )   _   s   \0
  const int STATSTR_max = 20 + 1 + 1 + NAME_MAX + 1 + 1 + 1 + 1;
  char buf[STATSTR_max];

  sprintf(path, "/proc/%d/stat", pid);

  fd = open(path, O_RDONLY);

  if (fd < 0) goto _shstat_fail;

  int readed = read(fd, buf, sizeof(buf));

  char* ch = buf;

  while(*ch != ' ' && *ch != '\0') ch++;
  if ( ch == '\0' || ( ch - &buf[0] < 1) ) goto _shstat_fail;
  ch++;
  while(*ch != ' ' && *ch != '\0') ch++;
  if ( ch == '\0' || ( ch - &buf[0] < 1 + 1 + 1 + 1 + 1) ) goto _shstat_fail;

  close(fd);
  return ch[1];

_shstat_fail:
  if (fd >= 0) close(fd);
  return -1;
}

size_t _writeall(int fd, const void* buf, size_t bytes)
{
  size_t writed = 0,
         rest = bytes;

  while(rest > 0) {
    ssize_t w = write(fd, ((const uint8_t*)buf)+writed, rest);

    if (w <= 0) {
      return writed;
    }

    writed += w;
    rest -= w;
  }

  return writed;
}

ssize_t _shread(int fd, int fdout)
{
  char buf[1024];
  ssize_t readed = read(fd, buf, sizeof(buf)-1);

  if (readed <= 0)
    return readed;

  buf[readed] = '\0';
  ssize_t sret = _writeall(fdout, buf, readed);

  if (sret != readed) {
    return -2;
  }

  return sret;
}

int _shwait(int pid, int fd, int fdout)
{
  char ch;

  while ( (ch = _shstat(pid)) != 'T' ) {
    if (ch == 'Z' || ch == -1) {
      return ch;
    }

    _shread(fd, fdout);
  }

  return ch;
}

int shfeed(int pid, int fd, int fdout, const char* msg)
{
  char buf[1024],
       cret;
  ssize_t sret,
          msglen = strlen(msg);

  if ( (cret = _shwait(pid, fd, fdout)) != 'T')
    goto shfeed_waitfailed;

  kill(pid, SIGCONT);

  sret = _writeall(fd, msg, msglen);

  if (sret == msglen)
    sret = write(fd, "\n", 1);
  else
    sret = -1;

  if (sret != 1) {
    perror("Can't feed pt with msg");
    goto shfeed_fail;
  }

  if ( (cret = _shwait(pid, fd, fdout)) != 'T')
    goto shfeed_waitfailed;

  ssize_t readed;

  while ( (readed = _shread(fd, fdout)) > 0);

  if (readed == -2) {
    perror("Can't print out results");
    goto shfeed_fail;
  }

  return 0;

shfeed_waitfailed:
  if (cret == -1) {
    perror("Shell status acquiring failed\n");
    goto shfeed_fail;
  } else if (cret == 'Z') {
    fprintf(stderr, "Shell terminated\n");
    goto shfeed_fail;
  }

shfeed_fail:
  return -1;
}


int shpawn(char *const cmd[], int* pid, int* fds)
{
  int iret,
      masterfd = -1,
      slavefd = -1;

  masterfd = open("/dev/ptmx", O_RDWR);

  if (masterfd < 0) {
    perror("Can't open pseudoterminal");
    goto spawn_sh_fail;
  }

  iret = grantpt(masterfd);

  if (iret != 0) {
    perror("Error granting permissions for pseudoterminal");
    goto spawn_sh_fail;
  }

  iret = unlockpt(masterfd);

  if (iret != 0) {
    perror("Error unlocking pseudoterminal");
    goto spawn_sh_fail;
  }

  char* slavename = ptsname(masterfd);

  slavefd = open(slavename, O_RDWR);

  if (slavefd < 0) {
    perror("Can't open slave end of pt");
    goto spawn_sh_fail;
  }

  int _pid = fork();

  if (_pid == 0) {
    close(masterfd);
    for (int i = 0; i < 3; i++) {
      close(i);
      dup2(slavefd, i);
    }
    close(slavefd);

    putenv("PROMPT_COMMAND=kill -STOP $$");

    execv(cmd[0], &cmd[1]);

    perror("Can't exec shell");
    exit(1);
  }

  int flags = fcntl(masterfd, F_GETFL, 0);
  fcntl(masterfd, F_SETFL, flags | O_NONBLOCK);

  *pid = _pid;
  fds[0] = slavefd;
  fds[1] = masterfd;

  return 0;

spawn_sh_fail:
  if (slavefd >= 0) close(slavefd);
  if (masterfd >= 0) close(masterfd);
  return -1;
}

#ifdef _SHPAWN_MAIN

int main(int argc, char **argv) 
{
  int fds[2],
      pid,
      ret,
      devnull;

  char* cmd[] = { "/bin/bash", "/bin/bash", "--norc", "--noprofile", NULL };

  ret = shpawn(cmd, &pid, fds);

  if (ret < 0) {
    fprintf(stderr, "Failed to spawn shell");
    return 1;
  }

  devnull = open("/dev/null", O_WRONLY);

  shfeed(pid, fds[1], devnull, "stty -echo");
  shfeed(pid, fds[1], devnull, "set +m");

  for (int i = 0; i < 100000; i++)
    shfeed(pid, fds[1], 1, "echo Foooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo");

  return 0;
}

#endif

