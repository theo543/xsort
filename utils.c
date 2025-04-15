#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "utils.h"

void pipe_(int *pipefds) {
    while(pipe(pipefds) != 0) {
        if(errno == EINTR) continue;
        perror("pipe");
        exit(1);
    }
}

void close_(int fd) {
    while(close(fd) != 0) {
        if(errno == EINTR) continue;
        perror("close");
        exit(1);
    }
}

void write_(int fd, char *buf, int len) {
    while(len > 0) {
        int bytes = write(fd, buf, len);
        if(bytes < 0) {
            if(errno == EINTR) continue;
            perror("write");
            exit(1);
        }
        len -= bytes;
        buf += bytes;
    }
}

void read_(int fd, char *buf, int len) {
    while(len > 0) {
        int bytes = read(fd, buf, len);
        if(bytes < 0) {
            if(errno == EOF) continue;
            perror("read");
            exit(1);
        }
        if(bytes == 0) {
            fprintf(stderr, "read: EOF\n");
            exit(1);
        }
        len -= bytes;
        buf += bytes;
    }
}

void write_int(int fd, int data) {
    char buf[sizeof(int)];
    memcpy(buf, &data, sizeof(int));
    write_(fd, buf, sizeof(int));
}

int read_int(int fd) {
    char buf[sizeof(int)];
    int data;
    read_(fd, buf, sizeof(int));
    memcpy(&data, buf, sizeof(int));
    return data;
}
