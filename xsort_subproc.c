#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <X11/Xlib.h>

void fake_expose(Display* dpy, Window win);

void draw_num_sphere(Display *display, Window window, GC gc, XFontStruct font, int centerX, int centerY, int radius, const char *numStr) {
    int numWidth = XTextWidth(&font, numStr, strlen(numStr));
    int numHeight = font.ascent + font.descent;
    int x = centerX - numWidth / 2;
    int y = centerY - numHeight / 2;
    XDrawArc(display, window, gc, centerX - radius, centerY - radius, 2 * radius, 2 * radius, 0, 360 * 64);
    XDrawString(display, window, gc, x, y + font.ascent, numStr, strlen(numStr));
}

void erase_num_sphere(Display *display, Window window, GC gc, int centerX, int centerY, int radius) {
    radius += 2;
    XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
    XFillArc(display, window, gc, centerX - radius, centerY - radius, 2 * radius, 2 * radius, 0, 360 * 64);
    XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
}

time_t get_time_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void run_sort(int64_t *buf, int bufLen, int actionIdx) {
    (void)actionIdx; // TODO: actionIdx specifies sorting algorithm
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

    int windowHeight = radius * 2 + 10 + font->ascent + font->descent + 10;
    int windowWidth = 400;

    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, windowWidth, windowHeight, 0, blackColor, whiteColor);
    char titleBuf[128];
    snprintf(titleBuf, sizeof(titleBuf), "XSort: sorting %d numbers", bufLen);
    XStoreName(display, window, titleBuf);
    XSelectInput(display, window, StructureNotifyMask);
    GC gc = XCreateGC(display, window, 0, NULL);
    XSetFont(display, gc, font->fid);
    Atom WM_DELETE_WINDOW = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &WM_DELETE_WINDOW, 1);
    XSelectInput(display, window, ExposureMask);
    XMapWindow(display, window);

    int fullWidth = (radius * 2 + 10) * bufLen + 10;
    Pixmap pixmap = XCreatePixmap(display, window, fullWidth, windowHeight, DefaultDepth(display, DefaultScreen(display)));
    XSetForeground(display, gc, whiteColor);
    XFillRectangle(display, pixmap, gc, 0, 0, fullWidth, windowHeight);
    XSetForeground(display, gc, blackColor);
    XSetBackground(display, gc, whiteColor);
    for(int i = 0; i < bufLen; i++) {
        int centerX = (radius * 2 + 10) * i + radius + 5;
        int centerY = windowHeight / 2;
        draw_num_sphere(display, pixmap, gc, *font, centerX, centerY, radius, numStr[i]);
    }

    int focusSphere = -1;
    int focusX = radius + 5;

    int maxFps = 30;
    time_t frameDuration = 1000000 / maxFps;
    time_t last_time = get_time_usec();

    for(;;) {
        if(XPending(display) == 0) {
            time_t time_usec = get_time_usec();
            time_t elapsed = time_usec - last_time;
            if(elapsed < frameDuration) {
                usleep(frameDuration / 2);
                continue;
            }
            last_time = time_usec;
            focusSphere = (focusSphere + 1);
            if(focusSphere >= bufLen) {
                focusSphere = 0;
            }
            focusX = (radius * 2 + 10) * focusSphere + radius + 5;
            fake_expose(display, window);
            continue;
        }

        XEvent e;
        XNextEvent(display, &e);
        if(e.type == ClientMessage && (Atom)e.xclient.data.l[0] == WM_DELETE_WINDOW) {
            break;
        }
        if(e.type == Expose) {
            XClearWindow(display, window);
            XCopyArea(display, pixmap, window, gc, focusX - windowWidth / 2, 0, windowWidth, windowHeight, 0, 0);
        }
    }

    XFreeGC(display, gc);
    XFreeFont(display, font);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    free(numStr);
    free(strBuf);
}
