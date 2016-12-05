#ifndef SHPAWN_H
#define SHPAWN_H

int shpawn(char *const cmd[], int* pid, int* fds);
int shfeed(int pid, int fd, int fdout, const char* msg);

#endif

