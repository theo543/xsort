#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include <unistd.h>
#include <X11/Xlib.h>

void draw_num_sphere(Display *display, Window window, GC gc, XFontStruct font, int centerX, int centerY, int radius, const char *numStr) {
    int numWidth = XTextWidth(&font, numStr, strlen(numStr));
    int numHeight = font.ascent + font.descent;
    int x = centerX - numWidth / 2;
    int y = centerY - numHeight / 2;
    XDrawArc(display, window, gc, centerX - radius, centerY - radius, 2 * radius, 2 * radius, 0, 360 * 64);
    XDrawString(display, window, gc, x, y + font.ascent, numStr, strlen(numStr));
}

void run_sort(int64_t *buf, int bufLen, int actionIdx) {
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

    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 400, windowHeight, 0, blackColor, whiteColor);
    XSelectInput(display, window, StructureNotifyMask);
    XMapWindow(display, window);

    GC gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, gc, blackColor);
    XSetFont(display, gc, font->fid);

    for(;;) {
        XEvent e;
        XNextEvent(display, &e);
        if(e.type == MapNotify) {
            break;
        }
    }

    int x = 10 + radius;
    int y = 10 + radius;
    for(int i = 0; i < bufLen; i++) {
        draw_num_sphere(display, window, gc, *font, x, y, radius, numStr[i]);
        x += radius * 2 + 10;
    }
    XFlush(display);
    sleep(5);

    XFreeGC(display, gc);
    XFreeFont(display, font);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    free(numStr);
    free(strBuf);
}
