#define _DEFAULT_SOURCE
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
#include <X11/Xutil.h>
#include <X11/keysym.h>

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
    int result = read_int(read_fd);
    if(result == -1) {
        // window closed before sort finished, exit early
        exit(0);
    }
    return result;
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

static void heap_sift_down(int r, int w, int len, int i) {
    // sift-down operation restores max-heap property when the root may be smaller than its children
    while(1) {
        int child1 = i * 2 + 1;
        int child2 = i * 2 + 2;
        int largest = i;
        if(child1 < len && smaller(r, w, largest, child1)) {
            largest = child1;
        }
        if(child2 < len && smaller(r, w, largest, child2)) {
            largest = child2;
        }
        if(largest == i) {
            break;
        }
        swap(w, i, largest);
        i = largest;
    }
}

static void heapify(int r, int w, int len) {
    // build max-heap from the bottom up
    // last non-leaf node is at (len - 2) / 2
    for(int i = (len - 2) / 2; i >= 0; i--) {
        heap_sift_down(r, w, len, i);
    }
}

static void heap_sort(int r, int w, int len) {
    heapify(r, w, len);
    for(int i = len - 1; i > 0; i--) {
        // extract largest element, move to end of array, reduce heap size by 1, restore max-heap property
        swap(w, 0, i);
        heap_sift_down(r, w, i, 0);
    }
}

typedef void (*sort_algo)(int, int, int);
static const sort_algo sort_algos[ALGO_LEN] = {
    bubble_sort,
    insert_sort,
    selection_sort,
    quick_sort,
    heap_sort,
    NULL,
};
const char * const algo_names[ALGO_LEN] = {
    "Bubble Sort",
    "Insertion Sort",
    "Selection Sort",
    "Quick Sort",
    "Heap Sort",
    "All",
};

static bool get_swap_request(int read_fd, int write_fd, int64_t *buf, int len, int *i, int *j, int *comparisions) {
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
        (*comparisions)++;
        int a = read_int(read_fd);
        int b = read_int(read_fd);
        assert(a >= 0 && a < len);
        assert(b >= 0 && b < len);
        write_int(write_fd, buf[a] < buf[b]);
    }
}

static void draw_num_sphere(Display *display, Window window, GC gc, XFontStruct *font, int sphereCenterX, int sphereCenterY, int radius, const char *numStr, int baseY) {
    int numWidth = XTextWidth(font, numStr, strlen(numStr));
    int numHeight = font->ascent + font->descent;
    int x = sphereCenterX - numWidth / 2;
    int y = sphereCenterY - numHeight / 2;
    XDrawArc(display, window, gc, sphereCenterX - radius, baseY + sphereCenterY - radius, 2 * radius, 2 * radius, 0, 360 * 64);
    XDrawString(display, window, gc, x, baseY + y + font->ascent, numStr, strlen(numStr));
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
    int progress;
    int end;
    int sphereIdx1;
    int sphereIdx2;
    enum { INIT, DOWN_1, RIGHT_1, UP_2, UP_1, LEFT_2, DOWN_2 } state;
};

static void update_anim_position(struct animation_state *anim) {
    assert(anim->progress <= anim->end);
    double percent = (double)anim->progress / anim->end;
    double x = anim->startX + (anim->targetX - anim->startX) * percent;
    double y = anim->startY + (anim->targetY - anim->startY) * percent;
    anim->x = (int)x;
    anim->y = (int)y;
}

static void launch_sorting_algorithm(int algoSelection, int bufLen, int *read_fd, int *write_fd) {
    assert(algoSelection >= 0 && algoSelection < ALGO_LEN - 1);
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

static void verify_sort(int64_t *buf, int bufLen, const char *algoName) {
    for(int i = 1; i < bufLen; i++) {
        if(buf[i - 1] > buf[i]) {
            fprintf(stderr, "%s: sort bug!\n", algoName);
            return;
        }
    }
    fprintf(stderr, "%s: sort completed successfully\n", algoName);
}

char *get_anim_str(struct animation_state *anim, char **numStr) {
    bool is_sphere_1 = anim->state == DOWN_1 || anim->state == RIGHT_1 || anim->state == UP_1;
    return numStr[is_sphere_1 ? anim->sphereIdx1 : anim->sphereIdx2];
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
    if(!display) {
        fprintf(stderr, "Failed to open display\n");
        exit(1);
    }
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

    int viewportHeight = (radius * 2 + 10) * 3;
    int statusPaneHeight = font->ascent + font->descent + 10;
    int windowHeight = viewportHeight + statusPaneHeight;
    int windowWidth = i_max(800, 10 * (radius * 2 + 10) + 10);

    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, windowWidth, windowHeight, 0, blackColor, whiteColor);
    char titleBuf[128];
    snprintf(titleBuf, sizeof(titleBuf), "XSort - sorting %d numbers with %s", bufLen, algo_names[algoSelection]);
    XClassHint *classHint = XAllocClassHint();
    if(classHint) {
        classHint->res_name = get_instance_name();
        classHint->res_class = "XSort (subprocess)";
        XSetClassHint(display, window, classHint);
        XFree(classHint);
    } else {
        fprintf(stderr, "XAllocClassHint failed\n");
    }
    XStoreName(display, window, titleBuf);
    XSelectInput(display, window, StructureNotifyMask);
    GC gc = XCreateGC(display, window, 0, NULL);
    GC erase_gc = XCreateGC(display, window, 0, NULL);
    if(!gc || !erase_gc) {
        fprintf(stderr, "Failed to create graphics context\n");
        exit(1);
    }
    XSetForeground(display, gc, blackColor);
    XSetFont(display, gc, font->fid);
    XSetForeground(display, erase_gc, whiteColor);
    Atom WM_DELETE_WINDOW = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &WM_DELETE_WINDOW, 1);
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(display, window);

    int fullWidth = (radius * 2 + 10) * bufLen + 10;
    Pixmap pixmap = XCreatePixmap(display, window, fullWidth, viewportHeight * 4, DefaultDepth(display, DefaultScreen(display)));
    if(!pixmap) {
        fprintf(stderr, "Failed to create pixmap\n");
        exit(1);
    }
    int baseY = viewportHeight * 2;
    int viewportY = 0;
    XFillRectangle(display, pixmap, erase_gc, 0, 0, fullWidth, viewportHeight * 4);

#define SPHERE_X(i) ((radius * 2 + 10) * (i) + radius + 5)

    for(int i = 0; i < bufLen; i++) {
        draw_num_sphere(display, pixmap, gc, font, SPHERE_X(i), viewportHeight / 2, radius, numStr[i], baseY);
    }

    time_t frameDuration = 1000000 / 60;
    time_t last_time = get_time_usec();
    int speed = 20;
    int vertical_anim_duration = 10 * speed;
    int horizontal_anim_duration = 20 * speed;
    struct animation_state anim = { .progress = 1, .end = 0, .state = DOWN_2, .sphereIdx1 = -1, .sphereIdx2 = -1 };
    bool animation_running = true;
    int focusX = SPHERE_X(0);
    int comparisions = 0;
    int swaps = 0;

    for(;;) {
        bool changed = false;
        time_t time_since_anim = get_time_usec() - last_time;
        if(animation_running && (time_since_anim >= frameDuration)) {
            if(anim.progress >= anim.end + 1) {
                // animation is done, go to next phase or get next swap request
                if(anim.sphereIdx1 != -1) {
                    erase_num_sphere(display, pixmap, erase_gc, anim.x, anim.y, radius, baseY);
                    draw_num_sphere(display, pixmap, gc, font, anim.targetX, anim.targetY, radius, get_anim_str(&anim, numStr), baseY);
                    anim.x = anim.targetX;
                    anim.y = anim.targetY;
                }
                if(anim.state == DOWN_2) {
                    if(anim.sphereIdx1 != -1) {
                        swaps++;
                        int64_t tmp_i = buf[anim.sphereIdx1];
                        buf[anim.sphereIdx1] = buf[anim.sphereIdx2];
                        buf[anim.sphereIdx2] = tmp_i;
                        char *tmp_str = numStr[anim.sphereIdx1];
                        numStr[anim.sphereIdx1] = numStr[anim.sphereIdx2];
                        numStr[anim.sphereIdx2] = tmp_str;
                    }

                    int nextSphere1, nextSphere2;
                    if(!get_swap_request(algorithm_read_fd, algorithm_write_fd, buf, bufLen, &nextSphere1, &nextSphere2, &comparisions)) {
                        animation_running = false;
                        close_(algorithm_read_fd);
                        close_(algorithm_write_fd);
                        verify_sort(buf, bufLen, algo_names[algoSelection]);
                        continue;
                    }
                    anim = (struct animation_state){.sphereIdx1 = nextSphere1, .sphereIdx2 = nextSphere2, .state = INIT};
                }

                switch(anim.state) {
                    case INIT:
                        // move sphere 1 down
                        anim.x = anim.startX = SPHERE_X(anim.sphereIdx1);
                        anim.y = anim.startY = viewportHeight / 2;
                        anim.targetX = anim.x;
                        anim.targetY = anim.y + radius * 2 + 10;
                        anim.end = vertical_anim_duration;
                        anim.state = DOWN_1;
                        break;
                    case DOWN_1:
                        // move sphere 1 right
                        anim.startY = anim.y;
                        anim.targetX = SPHERE_X(anim.sphereIdx2);
                        anim.end = horizontal_anim_duration;
                        anim.state = RIGHT_1;
                        break;
                    case RIGHT_1:
                        // move sphere 2 up to not overlap with sphere 1
                        anim.startX = anim.x;
                        anim.y = anim.startY = viewportHeight / 2;
                        anim.targetY = anim.y - radius * 2 - 10;
                        anim.end = vertical_anim_duration;
                        anim.state = UP_2;
                        break;
                    case UP_2:
                        // move sphere 1 up
                        anim.y = anim.startY = viewportHeight / 2 + radius * 2 + 10;
                        anim.targetY = viewportHeight / 2;
                        anim.end = vertical_anim_duration;
                        anim.state = UP_1;
                        break;
                    case UP_1:
                        // move sphere 2 left
                        anim.startX = anim.x;
                        anim.startY = anim.y = viewportHeight / 2 - radius * 2 - 10;
                        anim.targetX = SPHERE_X(anim.sphereIdx1);
                        anim.targetY = anim.y;
                        anim.end = horizontal_anim_duration;
                        anim.state = LEFT_2;
                        break;
                    case LEFT_2:
                        // move sphere 2 down
                        anim.startX = anim.x;
                        anim.startY = anim.y;
                        anim.targetY = viewportHeight / 2;
                        anim.end = vertical_anim_duration;
                        anim.state = DOWN_2;
                        break;
                    case DOWN_2:
                        assert(0 && "should not happen");
                }
                anim.progress = 0;
            }

            erase_num_sphere(display, pixmap, erase_gc, anim.x, anim.y, radius, baseY);
            update_anim_position(&anim);
            focusX = (int)((double)focusX + ((double)anim.x - focusX) / 10);
            draw_num_sphere(display, pixmap, gc, font, anim.x, anim.y, radius, get_anim_str(&anim, numStr), baseY);
            anim.progress += speed;

            last_time = get_time_usec();
            changed = true;
        }

        bool pending = XPending(display) > 0;
        if(animation_running && !pending && !changed) {
            time_t diff = frameDuration - time_since_anim;
            if(diff > 0) {
                usleep(diff);
            }
            continue;
        }

        XEvent e;
        // while the animation is running, never block on XNextEvent
        if(pending || !animation_running) {
            XNextEvent(display, &e);
        } else {
            assert(changed);
            e.type = Expose;
        }
        if(e.type == ClientMessage && (Atom)e.xclient.data.l[0] == WM_DELETE_WINDOW) {
            break;
        }
        if(e.type == ConfigureNotify) {
            windowWidth = e.xconfigure.width;
            windowHeight = e.xconfigure.height;
            viewportY = (windowHeight - viewportHeight - statusPaneHeight) / 2;
            changed = true;
        }
        int widthDiff = fullWidth - windowWidth;
        if(widthDiff < 0) {
            widthDiff = 0;
        }
        const int minFocusX = fullWidth / 2 - widthDiff / 2;
        const int maxFocusX = fullWidth / 2 + widthDiff / 2;
        focusX = i_max(minFocusX, i_min(focusX, maxFocusX));
        if(e.type == KeyPress) {
            KeySym keysym = XLookupKeysym(&e.xkey, 0);
            if(keysym == XK_Escape) {
                break;
            }
            if(keysym == XK_plus || keysym == XK_KP_Add) {
                speed++;
                changed = true;
            } else if(keysym == XK_minus || keysym == XK_KP_Subtract) {
                speed = i_max(0, speed - 1);
                changed = true;
            }
        }
        if(changed || e.type == Expose) {
            XClearWindow(display, window);
            XCopyArea(display, pixmap, window, gc, focusX - windowWidth / 2, baseY, windowWidth, viewportHeight, 0, viewportY);
            char statusBuf[256];
            snprintf(statusBuf, sizeof(statusBuf), "%s: %d comparisons, %d swaps. Speed: %d (change by pressing +/-)", algo_names[algoSelection], comparisions, swaps, speed);
            int statusX = (windowWidth - XTextWidth(font, statusBuf, strlen(statusBuf))) / 2;
            if(statusX < 0) {
                statusX = 0;
            }
            XDrawString(display, window, gc, statusX, viewportY + viewportHeight + font->ascent + font->descent + 5, statusBuf, strlen(statusBuf));
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
    if(animation_running) {
        // write a -1 to subprocess to prevent EOF error, if window was closed before sort finished
        // a SIGPIPE may happen if subprocess has finished already, but this process was going to exit right after anyway
        write_int(algorithm_write_fd, -1);
    }
}
