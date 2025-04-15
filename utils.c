#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <libgen.h>

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

int i_min(int a, int b) {
    return a < b ? a : b;
}

int i_max(int a, int b) {
    return a > b ? a : b;
}

static char *instance_name = "";

void set_instance_name(int argc, char **argv) {
    // ICCCM standard: instance name is set with "-name NAME" flag, or fallback to RESOURCE_NAME environment variable, or fallback to argv[0]
    for(int i = 1;i < argc - 1;i++) {
        if(strcmp(argv[i], "-name") == 0) {
            instance_name = argv[i + 1];
            return;
        }
    }
    char *name = getenv("RESOURCE_NAME");
    if(name != NULL) {
        instance_name = name;
        return;
    }
    if(argc > 0) {
        instance_name = basename(argv[0]);
        return;
    }
}

char *get_instance_name(void) {
    return instance_name;
}
