#define _DEFAULT_SOURCE
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <inttypes.h>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/random.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "utils.h"
#include "xsort_subproc.h"

static void drawButton(const char *text, int x, int y, Display *display, Window window, GC borderGC, GC fillGC, GC textGC, XFontStruct *font, int *width, int *height) {
    *width = XTextWidth(font, text, strlen(text)) + 10;
    *height = font->ascent + font->descent + 10;
    XDrawRectangle(display, window, borderGC, x, y, *width, *height);
    XFillRectangle(display, window, fillGC, x + 1, y + 1, *width - 2, *height - 2);
    XDrawString(display, window, textGC, x + 5, y + *height / 2 + 5, text, strlen(text));
}

struct Button {
    enum { LOAD, SAVE, LAUNCH, UP, DOWN, INSERT, DELETE, RANDOM } type;
    int x, y;
    int width, height;
};

static const char * const buttonText[] = {
    [LOAD] = "Load",
    [SAVE] = "Save",
    [LAUNCH] = "Launch",
    [UP] = "Up",
    [DOWN] = "Down",
    [INSERT] = "Insert",
    [DELETE] = "Delete",
    [RANDOM] = "Random"
};

static void insertAt(int64_t **buf, int *bufLen, int bufSelection, int64_t inputNr) {
    if(*bufLen == INT_MAX) {
        fprintf(stderr, "Buffer size too large\n");
        return;
    }
    *buf = reallocarray(*buf, *bufLen + 1, sizeof(int64_t));
    if(!*buf) {
        perror("realloc");
        exit(1);
    }
    for(int i = *bufLen; i > bufSelection; i--) {
        (*buf)[i] = (*buf)[i - 1];
    }
    (*buf)[bufSelection] = inputNr;
    (*bufLen)++;
}

static void deleteAt(int64_t **buf, int *bufLen, int bufSelection) {
    if(*bufLen == 0) {
        return;
    }
    for(int i = bufSelection; i < *bufLen - 1; i++) {
        (*buf)[i] = (*buf)[i + 1];
    }
    (*bufLen)--;
    *buf = reallocarray(*buf, *bufLen == 0 ? 1 : *bufLen, sizeof(int64_t));
    if(!*buf) {
        perror("realloc");
        exit(1);
    }
}

static const char * const buf_file_name = "xsort_buf.txt";
static void saveBuffer(int64_t *buf, int bufLen) {
    FILE *file = fopen(buf_file_name, "w");
    if(!file) {
        perror("fopen");
        return;
    }
    for(int i = 0; i < bufLen; i++) {
        if(fprintf(file, "%" PRId64 "\n", buf[i]) < 0) {
            perror("fprintf");
            fclose(file);
            return;
        }
    }
    if(fclose(file) != 0) {
        perror("fclose");
        return;
    }
}

static int64_t *loadBuffer(int *bufLen) {
    FILE *file = fopen(buf_file_name, "r");
    int64_t *buf = NULL;
    size_t len = 0;

    if(!file) {
        perror("fopen");
        goto fail;
    }

    while(1) {
        int64_t num;
        if(fscanf(file, "%" SCNd64 "\n", &num) != 1) {
            if(feof(file)) {
                break;
            }
            perror("fscanf");
            goto fail2;
        }
        if(len == 0 || ((len & (len - 1)) == 0)) {
            // realloc when len is 0 or a power of 2
            int size_t_bits = sizeof(size_t) * 8;
            if(len & (((size_t)1) << (size_t_bits - 1))) {
                fprintf(stderr, "Buffer size too large\n");
                goto fail2;
            }
            buf = reallocarray(buf, len == 0 ? 1 : len << 1, sizeof(int64_t));
            if(!buf) {
                perror("reallocarray");
                goto fail2;
            }
        }
        buf[len++] = num;
    }
    if(fclose(file) != 0) {
        perror("fclose");
    }
    *bufLen = len;
    return buf;

    fail2:
    if(fclose(file) != 0) {
        perror("fclose");
    }
    fail:
    if(buf) {
        free(buf);
    }
    *bufLen = 0;
    return NULL;
}

int launch_fork_server(void) {
    int fork_server_fd[2];
    pipe_(fork_server_fd);
    pid_t fork_server_pid = fork();
    if(fork_server_pid < 0) {
        perror("fork");
        exit(1);
    }
    if(fork_server_pid != 0) {
        close_(fork_server_fd[0]);
        return fork_server_fd[1];
    }
    close_(fork_server_fd[1]);
    char *buf = NULL;
    while(1) {
        int bufLen = read_int(fork_server_fd[0]);
        if(bufLen == -1) {
            close_(fork_server_fd[0]);
            exit(0);
        }
        buf = reallocarray(buf, bufLen, sizeof(int64_t));
        if(!buf) {
            perror("reallocarray");
            exit(1);
        }
        read_(fork_server_fd[0], (char*)buf, bufLen * sizeof(int64_t));
        pid_t sort_pid = fork();
        if(sort_pid < 0) {
            perror("fork");
            exit(1);
        }
        if(sort_pid == 0) {
            close_(fork_server_fd[0]);
            run_sort((int64_t*)buf, bufLen, 0);
            free(buf);
            exit(0);
        }
    }
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);
    int fork_server_fd = launch_fork_server();

    struct Button buttons[] = {
        (struct Button){.type = LOAD},
        (struct Button){.type = SAVE},
        (struct Button){.type = LAUNCH},
        (struct Button){.type = UP},
        (struct Button){.type = DOWN},
        (struct Button){.type = INSERT},
        (struct Button){.type = DELETE},
        (struct Button){.type = RANDOM}
    };
    const int buttonsLen = sizeof(buttons) / sizeof(buttons[0]);

    Display *display = XOpenDisplay(NULL);
    int blackColor = BlackPixel(display, DefaultScreen(display));
    int whiteColor = WhitePixel(display, DefaultScreen(display));
    XVisualInfo vInfo;
    if(!XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &vInfo)) {
        fprintf(stderr, "No matching visual found\n");
        return 1;
    }
    Colormap colormap = XCreateColormap(display, DefaultRootWindow(display), vInfo.visual, AllocNone);
    int grayColor;
    int lightGrayColor;
    int blueColor;
    XColor grayExact, lightGrayExact, grayDisplay, lightGrayDisplay, blueDisplay, blueExact;
    if(!XAllocNamedColor(display, colormap, "gray", &grayExact, &grayDisplay)) {
        fprintf(stderr, "Failed to allocate gray color\n");
        return 1;
    }
    if(!XAllocNamedColor(display, colormap, "lightgray", &lightGrayExact, &lightGrayDisplay)) {
        fprintf(stderr, "Failed to allocate light gray color\n");
        return 1;
    }
    if(!XAllocNamedColor(display, colormap, "blue", &blueExact, &blueDisplay)) {
        fprintf(stderr, "Failed to allocate blue color\n");
        return 1;
    }
    grayColor = grayExact.pixel;
    lightGrayColor = lightGrayExact.pixel;
    blueColor = blueExact.pixel;

    const char *fontQuery = "fixed";
    XFontStruct *font = XLoadQueryFont(display, fontQuery);
    if (!font) {
        fprintf(stderr, "Failed to load font \"%s\"\n", fontQuery);
        return 1;
    }

    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 400, 400, 0, blackColor, lightGrayColor);
    XStoreName(display, window, "XSort");
    XSelectInput(display, window, StructureNotifyMask);
    XMapWindow(display, window);

    GC lineGC = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, lineGC, blackColor);
    GC borderGC = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, borderGC, blackColor);
    GC fillGC = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, fillGC, grayColor);
    GC textGC = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, textGC, blackColor);
    GC textAreaGC = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, textAreaGC, whiteColor);
    GC selectedTextGC = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, selectedTextGC, blueColor);
    XSetFont(display, lineGC, font->fid);
    XSetFont(display, borderGC, font->fid);

    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask);
    Atom WM_DELETE_WINDOW = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &WM_DELETE_WINDOW, 1);

    XFlush(display);

    int64_t inputNr = 0;

    int64_t *buf = malloc(sizeof(int64_t));
    if(!buf) {
        perror("malloc");
        exit(1);
    }
    buf[0] = 0;
    int bufLen = 1;
    int bufSelection = 0;

    for(;;) {
        XEvent e;
        XNextEvent(display, &e);
        if(e.type == ClientMessage && (Atom)e.xclient.data.l[0] == WM_DELETE_WINDOW) {
            break;
        }
        if(e.type == Expose) {
            XClearWindow(display, window);
            int y = 10;
            for (int i = 0; i < buttonsLen; i++) {
                buttons[i].y = y;
                drawButton(buttonText[buttons[i].type], buttons[i].x, buttons[i].y, display, window, borderGC, fillGC, textGC, font, &buttons[i].width, &buttons[i].height);
                if(i != buttonsLen - 1) {
                    buttons[i + 1].x = buttons[i].x + buttons[i].width + 10;
                }
            }
            y = buttons[0].y + buttons[0].height + 10;
            int textAreaHeight = font->ascent + font->descent + 2;
            XDrawRectangle(display, window, borderGC, 10, y, 380, textAreaHeight);
            XFillRectangle(display, window, textAreaGC, 11, y + 1, 379, textAreaHeight - 2);
            char textBuf[255];
            sprintf(textBuf, "%" PRId64, inputNr);
            XDrawString(display, window, textGC, 15, y + textAreaHeight / 2 + 5, textBuf, strlen(textBuf));
            XFlush(display);
            y = buttons[0].y + buttons[0].height + 10 + textAreaHeight + 20;
            sprintf(textBuf, "Edit buffer contains %d number%s to be sorted", bufLen, bufLen == 1 ? "" : "s");
            XDrawString(display, window, textGC, 10, y, textBuf, strlen(textBuf));
            y = buttons[0].y + buttons[0].height + 10 + textAreaHeight + 20 + font->ascent + font->descent + 20;
            const char *dots = "....";
            int numStart = bufSelection;
            int numEnd = bufSelection + 10;
            for(int i = 0;i < 5 && numStart > 0; i++) {
                numStart--;
                numEnd--;
            }
            if(numEnd > bufLen) {
                numEnd = bufLen;
            }
            if(numStart > 0) {
                XDrawString(display, window, textGC, 10, y, dots, strlen(dots));
            }
            y += font->ascent + font->descent + 5;
            int arrowX;
            int arrowY;
            for(int i = numStart < 0 ? 0 : numStart; i < numEnd && i < bufLen; i++) {
                sprintf(textBuf, "%" PRId64, buf[i]);
                XDrawString(display, window, i == bufSelection ? selectedTextGC : textGC, 10, y, textBuf, strlen(textBuf));
                if(i == bufSelection) {
                    // draw arrow pointing to insert position from the right
                    arrowX = 10 + XTextWidth(font, textBuf, strlen(textBuf)) + 5;
                    arrowY = y - font->ascent;
                }
                y += font->ascent + font->descent + 5;
            }
            if(numEnd < bufLen) {
                XDrawString(display, window, textGC, 10, y, dots, strlen(dots));
            }
            if(bufSelection == bufLen) {
                arrowX = 10;
                arrowY = y - font->ascent;
            }
            XDrawLine(display, window, lineGC, arrowX, arrowY, arrowX + 10, arrowY);
            XDrawLine(display, window, lineGC, arrowX, arrowY, arrowX + 5, arrowY - 5);
            XDrawLine(display, window, lineGC, arrowX, arrowY, arrowX + 5, arrowY + 5);
        }
        if(e.type == KeyPress) {
            KeySym keysym = XLookupKeysym(&e.xkey, 0);
            if(keysym == XK_Escape) {
                break;
            }
            if(keysym == XK_BackSpace) {
                inputNr /= 10;
                fake_expose(display, window);
                continue;
            }
            char key[2] = "0";
            XLookupString(&e.xkey, key, sizeof(key), NULL, NULL);
            if(isdigit(key[0])) {
                if(inputNr >= 0 && inputNr < INT64_MAX / 10) {
                    inputNr = inputNr * 10 + (key[0] - '0');
                } else if(inputNr < 0 && inputNr > INT64_MIN / 10) {
                    inputNr = inputNr * 10 - (key[0] - '0');
                } else if(inputNr > 0) {
                    inputNr = INT64_MAX;
                } else {
                    inputNr = INT64_MIN;
                }
                fake_expose(display, window);
            } else if(key[0] == '-') {
                if(inputNr == INT64_MIN) {
                    inputNr = INT64_MAX;
                } else {
                    inputNr = -inputNr;
                }
                fake_expose(display, window);
            }
        }
        if(e.type == ButtonPress) {
            if(e.xbutton.button != Button1) {
                continue;
            }
            int pressedButton = -1;
            int x = e.xbutton.x;
            int y = e.xbutton.y;
            for(int i = 0;i < buttonsLen; i++) {
                if(x >= buttons[i].x && x <= buttons[i].x + buttons[i].width && y >= buttons[i].y && y <= buttons[i].y + buttons[i].height) {
                    pressedButton = i;
                    break;
                }
            }
            if(pressedButton == -1) {
                continue;
            }
            switch(buttons[pressedButton].type) {
                case LOAD:
                    fprintf(stderr, "Loading from %s\n", buf_file_name);
                    free(buf);
                    buf = loadBuffer(&bufLen);
                    if(bufSelection > bufLen) {
                        bufSelection = bufLen;
                    }
                    break;
                case SAVE:
                    fprintf(stderr, "Saving to %s\n", buf_file_name);
                    saveBuffer(buf, bufLen);
                    break;
                case LAUNCH:
                    if(bufLen == 0) {
                        fprintf(stderr, "Buffer is empty\n");
                        break;
                    }
                    write_int(fork_server_fd, bufLen);
                    write_(fork_server_fd, (char*)buf, bufLen * sizeof(int64_t));
                    break;
                case UP:
                    bufSelection = bufSelection == 0 ? 0 : bufSelection - 1;
                    break;
                case DOWN:
                    bufSelection = bufSelection == bufLen ? bufLen : bufSelection + 1;
                    break;
                case INSERT:
                    insertAt(&buf, &bufLen, bufSelection, inputNr);
                    inputNr = inputNr == INT64_MAX ? INT64_MIN : inputNr + 1;
                    bufSelection++;
                    break;
                case DELETE:
                    if(bufSelection == bufLen) {
                        fprintf(stderr, "Nothing to delete at %d\n", bufSelection);
                        break;
                    }
                    deleteAt(&buf, &bufLen, bufSelection);
                    bufSelection = bufSelection == 0 ? 0 : bufSelection - 1;
                    break;
                case RANDOM:
                    while(getrandom(buf, bufLen * sizeof(int64_t), 0) < 0) {
                        if(errno == EINTR) continue;
                        perror("getrandom");
                        break;
                    }
                    for(int i = 0; i < bufLen; i++) {
                        buf[i] %= 100;
                    }
                    break;
            }
            fake_expose(display, window);
        }
    }

    XFreeGC(display, lineGC);
    XFreeGC(display, borderGC);
    XFreeGC(display, fillGC);
    XFreeGC(display, textGC);
    XFreeGC(display, textAreaGC);
    XFreeGC(display, selectedTextGC);
    XFreeFont(display, font);
    XFreeColormap(display, colormap);
    XCloseDisplay(display);
    if(buf) free(buf);
    write_int(fork_server_fd, -1);
    close_(fork_server_fd);
}
