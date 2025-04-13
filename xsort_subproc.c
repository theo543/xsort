#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>

#include <unistd.h>
#include <X11/Xlib.h>

#include "utils.h"
#include "xsort_subproc.h"

const int64_t COMPARE = 0, SWAP = 1, FINISH = 2;

static void swap(int fd, int i, int j) {
    write_int(fd, SWAP);
    write_int(fd, i);
    write_int(fd, j);
}

static int compare(int read_fd, int write_fd, int i, int j) {
    write_int(write_fd, COMPARE);
    write_int(write_fd, i);
    write_int(write_fd, j);
    return read_int(read_fd);
}

static void bubble_sort(int read_fd, int write_fd, int len) {
    bool swapped = true;
    while(swapped) {
        swapped = false;
        for(int x = 0;x < len - 1;x++) {
            if(compare(read_fd, write_fd, x + 1, x)) {
                swap(write_fd, x, x + 1);
                swapped = true;
            }
        }
    };
    write_int(write_fd, FINISH);
}

static bool get_swap_request(int read_fd, int write_fd, int64_t *buf, int len, int *i, int *j) {
    while(1) {
        int request = read_int(read_fd);
        if(request == FINISH) {
            return false;
        }
        if(request == SWAP) {
            *i = read_int(read_fd);
            *j = read_int(read_fd);
            return true;
        }
        assert(request == COMPARE);
        int a = read_int(read_fd);
        int b = read_int(read_fd);
        assert(a >= 0 && a < len);
        assert(b >= 0 && b < len);
        write_int(write_fd, buf[a] < buf[b]);
    }
}

static void draw_num_sphere(Display *display, Window window, GC gc, XFontStruct font, int sphereCenterX, int sphereCenterY, int radius, const char *numStr, int baseY) {
    int numWidth = XTextWidth(&font, numStr, strlen(numStr));
    int numHeight = font.ascent + font.descent;
    int x = sphereCenterX - numWidth / 2;
    int y = sphereCenterY - numHeight / 2;
    XDrawArc(display, window, gc, sphereCenterX - radius, baseY + sphereCenterY - radius, 2 * radius, 2 * radius, 0, 360 * 64);
    XDrawString(display, window, gc, x, baseY + y + font.ascent, numStr, strlen(numStr));
}

static void erase_num_sphere(Display *display, Window window, GC gc, int sphereCenterX, int sphereCenterY, int radius, int baseY) {
    radius += 3;
    XFillArc(display, window, gc, sphereCenterX - radius, baseY + sphereCenterY - radius, 2 * radius, 2 * radius, 0, 360 * 64);
}

static time_t get_time_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

struct animation_state {
    int x, y;
    int startX, startY;
    int targetX, targetY;
    int frame;
    int frames;
    int sphereIdx1;
    int sphereIdx2;
    enum { INIT, DOWN_1, RIGHT_1, UP_2, UP_1, LEFT_2, DOWN_2 } state;
};

static void update_anim_position(struct animation_state *anim) {
    assert(anim->frame <= anim->frames);
    double percent = (double)anim->frame / anim->frames;
    double x = anim->startX + (anim->targetX - anim->startX) * percent;
    double y = anim->startY + (anim->targetY - anim->startY) * percent;
    anim->x = (int)x;
    anim->y = (int)y;
}

static void launch_sorting_algorithm(int actionIdx, int bufLen, int *read_fd, int *write_fd) {
    (void)actionIdx; // TODO: actionIdx specifies sorting algorithm
    int parent_to_child[2];
    int child_to_parent[2];
    pipe_(parent_to_child);
    pipe_(child_to_parent);
    pid_t pid = fork();
    if(pid == -1) {
        perror("fork");
        exit(1);
    }
    if(pid != 0) {
        *read_fd = child_to_parent[0];
        *write_fd = parent_to_child[1];
        close_(parent_to_child[0]);
        close_(child_to_parent[1]);
        return;
    }
    close_(parent_to_child[1]);
    close_(child_to_parent[0]);
    bubble_sort(parent_to_child[0], child_to_parent[1], bufLen);
    close_(parent_to_child[0]);
    close_(child_to_parent[1]);
    exit(0);
}

void run_sort(int64_t *buf, int bufLen, int actionIdx) {
    int algorithm_read_fd, algorithm_write_fd;
    launch_sorting_algorithm(actionIdx, bufLen, &algorithm_read_fd, &algorithm_write_fd);

    char **numStr = reallocarray(NULL, bufLen, sizeof(char*));
    char *strBuf = reallocarray(NULL, bufLen, 22);
    char *bufPtr = strBuf;
    if(!numStr) {
        perror("reallocarray");
        exit(1);
    }
    if(!strBuf) {
        perror("reallocarray");
        exit(1);
    }
    for(int i = 0; i < bufLen; i++) {
        numStr[i] = bufPtr;
        int len = sprintf(bufPtr, "%" PRId64, buf[i]);
        if(len < 0) {
            perror("sprintf");
            exit(1);
        }
        bufPtr += len + 1;
    }

    Display *display = XOpenDisplay(NULL);
    int blackColor = BlackPixel(display, DefaultScreen(display));
    int whiteColor = WhitePixel(display, DefaultScreen(display));

    const char *fontQuery = "fixed";
    XFontStruct *font = XLoadQueryFont(display, fontQuery);
    if (!font) {
        fprintf(stderr, "Failed to load font \"%s\"\n", fontQuery);
        exit(1);
    }

    int maxWidth = 0;
    for(int i = 0; i < bufLen; i++) {
        int width = XTextWidth(font, numStr[i], strlen(numStr[i]));
        if(width > maxWidth) {
            maxWidth = width;
        }
    }
    int radius = maxWidth / 2 + 5;

    int windowHeight = (radius * 2 + 10) * 3;
    const int minWidth = 400;
    int windowWidth = (10 * (radius * 2 + 10) + 10);
    if(windowWidth < minWidth) {
        windowWidth = minWidth;
    }

    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, windowWidth, windowHeight, 0, blackColor, whiteColor);
    char titleBuf[128];
    snprintf(titleBuf, sizeof(titleBuf), "XSort: sorting %d numbers", bufLen);
    XStoreName(display, window, titleBuf);
    XSelectInput(display, window, StructureNotifyMask);
    GC gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, gc, blackColor);
    XSetFont(display, gc, font->fid);
    GC erase_gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, erase_gc, whiteColor);
    Atom WM_DELETE_WINDOW = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &WM_DELETE_WINDOW, 1);
    XSelectInput(display, window, ExposureMask);
    XMapWindow(display, window);

    int fullWidth = (radius * 2 + 10) * bufLen + 10;
    Pixmap pixmap = XCreatePixmap(display, window, fullWidth, windowHeight * 4, DefaultDepth(display, DefaultScreen(display)));
    int baseY = windowHeight * 2;
    XFillRectangle(display, pixmap, erase_gc, 0, 0, fullWidth, windowHeight * 4);

#define SPHERE_X(i) ((radius * 2 + 10) * (i) + radius + 5)

    for(int i = 0; i < bufLen; i++) {
        draw_num_sphere(display, pixmap, gc, *font, SPHERE_X(i), windowHeight / 2, radius, numStr[i], baseY);
    }

    time_t frameDuration = 1000000 / 60;
    time_t last_time = get_time_usec();
    int frames_per_animation_vertical = 10;
    int frames_per_animation_horizontal = 20;
    struct animation_state anim = { .frame = 1, .frames = 0, .state = DOWN_2, .sphereIdx1 = -1, .sphereIdx2 = -1 };
    bool animation_running = true;
    int focusX = SPHERE_X(0);

    for(;;) {
        time_t time_since_anim = get_time_usec() - last_time;
        if(animation_running && (time_since_anim >= frameDuration)) {
            if(anim.frame == anim.frames + 1) {
                // animation is done, go to next phase or get next swap request

                if(anim.state == DOWN_2) {
                    if(anim.sphereIdx1 != -1) {
                        int64_t tmp_i = buf[anim.sphereIdx1];
                        buf[anim.sphereIdx1] = buf[anim.sphereIdx2];
                        buf[anim.sphereIdx2] = tmp_i;
                        char *tmp_str = numStr[anim.sphereIdx1];
                        numStr[anim.sphereIdx1] = numStr[anim.sphereIdx2];
                        numStr[anim.sphereIdx2] = tmp_str;
                    }

                    int nextSphere1, nextSphere2;
                    if(!get_swap_request(algorithm_read_fd, algorithm_write_fd, buf, bufLen, &nextSphere1, &nextSphere2)) {
                        animation_running = false;
                        close_(algorithm_read_fd);
                        close_(algorithm_write_fd);
                        continue;
                    }
                    anim = (struct animation_state){.sphereIdx1 = nextSphere1, .sphereIdx2 = nextSphere2, .state = INIT};
                }

                // set startX, startY, targetX, targetY, frames for next phase of animation
                switch(anim.state) {
                    case INIT:
                        // move sphere 1 down
                        anim.x = anim.startX = SPHERE_X(anim.sphereIdx1);
                        anim.y = anim.startY = windowHeight / 2;
                        anim.targetX = anim.x;
                        anim.targetY = anim.y + radius * 2 + 10;
                        anim.frames = frames_per_animation_vertical;
                        anim.state = DOWN_1;
                        break;
                    case DOWN_1:
                        // move sphere 1 right
                        anim.startY = anim.y;
                        anim.targetX = SPHERE_X(anim.sphereIdx2);
                        anim.frames = frames_per_animation_horizontal;
                        anim.state = RIGHT_1;
                        break;
                    case RIGHT_1:
                        // move sphere 2 up to not overlap with sphere 1
                        anim.startX = anim.x;
                        anim.y = anim.startY = windowHeight / 2;
                        anim.targetY = anim.y - radius * 2 - 10;
                        anim.frames = frames_per_animation_vertical;
                        anim.state = UP_2;
                        break;
                    case UP_2:
                        // move sphere 1 up
                        anim.y = anim.startY = windowHeight / 2 + radius * 2 + 10;
                        anim.targetY = windowHeight / 2;
                        anim.frames = frames_per_animation_vertical;
                        anim.state = UP_1;
                        break;
                    case UP_1:
                        // move sphere 2 left
                        anim.startX = anim.x;
                        anim.startY = anim.y = windowHeight / 2 - radius * 2 - 10;
                        anim.targetX = SPHERE_X(anim.sphereIdx1);
                        anim.targetY = anim.y;
                        anim.frames = frames_per_animation_horizontal;
                        anim.state = LEFT_2;
                        break;
                    case LEFT_2:
                        // move sphere 2 down
                        anim.startX = anim.x;
                        anim.startY = anim.y;
                        anim.targetY = windowHeight / 2;
                        anim.frames = frames_per_animation_vertical;
                        anim.state = DOWN_2;
                        break;
                    case DOWN_2:
                        assert(0 && "should not happen");
                }
                anim.frame = 0;
            }

            erase_num_sphere(display, pixmap, erase_gc, anim.x, anim.y, radius, baseY);
            update_anim_position(&anim);
            focusX = (int)((double)focusX + ((double)anim.x - focusX) / 10);
            const bool is_sphere_1[] = {[DOWN_1] = true, [RIGHT_1] = true, [UP_2] = false, [UP_1] = true, [LEFT_2] = false, [DOWN_2] = false};
            const char *str = numStr[is_sphere_1[anim.state] ? anim.sphereIdx1 : anim.sphereIdx2];
            draw_num_sphere(display, pixmap, gc, *font, anim.x, anim.y, radius, str, baseY);
            anim.frame++;

            last_time = get_time_usec();
            fake_expose(display, window);
        }

        if(animation_running && (XPending(display) == 0)) {
            usleep(frameDuration - time_since_anim);
            continue;
        }

        XEvent e;
        XNextEvent(display, &e);
        if(e.type == ClientMessage && (Atom)e.xclient.data.l[0] == WM_DELETE_WINDOW) {
            break;
        }
        if(e.type == Expose) {
            XClearWindow(display, window);
            XCopyArea(display, pixmap, window, gc, focusX - windowWidth / 2, baseY, windowWidth, windowHeight, 0, 0);
            XFlush(display);
        }
    }

    XFreeGC(display, gc);
    XFreeGC(display, erase_gc);
    XFreeFont(display, font);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    free(numStr);
    free(strBuf);
}
