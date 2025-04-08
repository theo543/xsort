#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <unistd.h>
#include <X11/Xlib.h>

void run_sort(int64_t *buf, int bufLen, int actionIdx) {
    Display *display = XOpenDisplay(NULL);
    int blackColor = BlackPixel(display, DefaultScreen(display));
    int whiteColor = WhitePixel(display, DefaultScreen(display));

    const char *fontQuery = "fixed";
    XFontStruct *font = XLoadQueryFont(display, fontQuery);
    if (!font) {
        fprintf(stderr, "Failed to load font \"%s\"\n", fontQuery);
        exit(1);
    }

    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 400, 400, 0, blackColor, whiteColor);
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

    XDrawString(display, window, gc, 10, 20, "TODO", 4);
    XFlush(display);
    sleep(2);

    XFreeGC(display, gc);
    XFreeFont(display, font);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
}
