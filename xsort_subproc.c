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

static const int64_t COMPARE_SMALLER = 0, SWAP = 1, FINISH = 2;

static void swap(int write_fd, int i, int j) {
    if(i == j) {
        return;
    }
    write_int(write_fd, SWAP);
    write_int(write_fd, i);
    write_int(write_fd, j);
}

static int smaller(int read_fd, int write_fd, int i, int j) {
    write_int(write_fd, COMPARE_SMALLER);
    write_int(write_fd, i);
    write_int(write_fd, j);
    return read_int(read_fd);
}

static void bubble_sort(int r, int w, int len) {
    bool swapped = true;
    while(swapped) {
        swapped = false;
        for(int x = 0;x < len - 1;x++) {
            if(smaller(r, w, x + 1, x)) {
                swap(w, x, x + 1);
                swapped = true;
            }
        }
    };
}

static void insert_sort(int r, int w, int len) {
    for(int x = 1;x < len;x++) {
        for(int y = x;y > 0;y--) {
            if(smaller(r, w, y, y - 1)) {
                swap(w, y, y - 1);
            }
        }
    }
}

static void selection_sort(int r, int w, int len) {
    for(int x = 0;x < len - 1;x++) {
        int min = x;
        for(int y = x + 1;y < len;y++) {
            if(smaller(r, w, y, min)) {
                min = y;
            }
        }
        if(min != x) {
            swap(w, x, min);
        }
    }
}

static void quick_sort_rec(int r, int w, int start, int end) {
    if(start >= end) {
        return;
    }
    if(start + 1 == end) {
        if(smaller(r, w, end, start)) {
            swap(w, end, start);
        }
        return;
    }
    swap(w, start, start + (end - start) / 2);
    int i = start + 1;
    int j = end;
    while(i <= j) {
        if(smaller(r, w, i, start)) {
            i++;
        } else if(!smaller(r, w, j, start)) {
            j--;
        } else {
            swap(w, i, j);
            i++;
            j--;
        }
    }
    swap(w, start, j);
    quick_sort_rec(r, w, start, j - 1);
    quick_sort_rec(r, w, j + 1, end);
}

static void quick_sort(int r, int w, int len) {
    quick_sort_rec(r, w, 0, len - 1);
}

typedef void (*sort_algo)(int, int, int);
static const sort_algo sort_algos[ALGO_LEN] = {
    bubble_sort,
    insert_sort,
    selection_sort,
    quick_sort,
};
const char * const algo_names[ALGO_LEN] = {
    "Bubble Sort",
    "Insertion Sort",
    "Selection Sort",
    "Quick Sort",
};

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
        assert(request == COMPARE_SMALLER);
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

static void launch_sorting_algorithm(int algoSelection, int bufLen, int *read_fd, int *write_fd) {
    assert(algoSelection >= 0 && algoSelection < ALGO_LEN);
    sort_algo sort = sort_algos[algoSelection];

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
    sort(parent_to_child[0], child_to_parent[1], bufLen);
    write_int(child_to_parent[1], FINISH);
    close_(parent_to_child[0]);
    close_(child_to_parent[1]);
    exit(0);
}

void verify_sort(int64_t *buf, int bufLen, const char *algoName) {
    for(int i = 1; i < bufLen; i++) {
        if(buf[i - 1] > buf[i]) {
            fprintf(stderr, "%s: sort bug!\n", algoName);
            return;
        }
    }
    fprintf(stderr, "%s: sort completed successfully\n", algoName);
}

void run_sort(int64_t *buf, int bufLen, int algoSelection) {
    int algorithm_read_fd, algorithm_write_fd;
    launch_sorting_algorithm(algoSelection, bufLen, &algorithm_read_fd, &algorithm_write_fd);

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
    const int minWidth = 1600;
    int windowWidth = (10 * (radius * 2 + 10) + 10);
    if(windowWidth < minWidth) {
        windowWidth = minWidth;
    }

    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, windowWidth, windowHeight, 0, blackColor, whiteColor);
    char titleBuf[128];
    snprintf(titleBuf, sizeof(titleBuf), "XSort - sorting %d numbers with %s", bufLen, algo_names[algoSelection]);
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
                        verify_sort(buf, bufLen, algo_names[algoSelection]);
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
