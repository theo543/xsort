#define _DEFAULT_SOURCE
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <inttypes.h>
#include <stdbool.h>
#include <assert.h>

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

static void drawRadioButton(const char *text, int x, int y, Display *display, Window window, GC borderGC, GC whiteGC, GC textGC, XFontStruct *font, int *width, int *height, bool selected) {
    const int circleRadius = font->ascent + font->descent;
    *width = XTextWidth(font, text, strlen(text)) + circleRadius + 10;
    *height = circleRadius;
    XDrawArc(display, window, borderGC, x, y, circleRadius, circleRadius, 0, 360 * 64);
    XFillArc(display, window, whiteGC, x + 1, y + 1, circleRadius - 2, circleRadius - 2, 0, 360 * 64);
    XDrawString(display, window, textGC, x + circleRadius + 5, y + circleRadius / 2 + 5, text, strlen(text));
    if(selected) {
        XFillArc(display, window, borderGC, x + 3, y + 3, circleRadius - 6, circleRadius - 6, 0, 360 * 64);
    }
}

struct Button {
    enum ButtonType { LOAD, SAVE, LAUNCH, UP, DOWN, INSERT, DELETE, RANDOM, ALGO_SELECT } type;
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
        perror("reallocarray");
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
        perror("reallocarray");
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

static void spawn_sort(char *buf, int bufLen, int algoSelection) {
    pid_t sort_pid = fork();
    if(sort_pid < 0) {
        perror("fork");
    }
    if(sort_pid == 0) {
        run_sort((int64_t*)buf, bufLen, algoSelection);
        exit(0);
    }
}

static int launch_fork_server(void) {
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
        int algoSelection = read_int(fork_server_fd[0]);
        if(algoSelection == -1) {
            close_(fork_server_fd[0]);
            if(buf) free(buf);
            exit(0);
        }
        int bufLen = read_int(fork_server_fd[0]);
        buf = reallocarray(buf, bufLen, sizeof(int64_t));
        if(!buf) {
            perror("reallocarray");
            exit(1);
        }
        read_(fork_server_fd[0], buf, bufLen * sizeof(int64_t));
        if(algoSelection != ALGO_LEN - 1) {
            spawn_sort(buf, bufLen, algoSelection);
        } else {
            for(int i = 0; i < ALGO_LEN - 1; i++) {
                spawn_sort(buf, bufLen, i);
            }
        }
    }
}

static bool in_bounds(int x, int y, struct Button *btn) {
    return x >= btn->x && x <= (btn->x + btn->width) && y >= btn->y && y <= (btn->y + btn->height);
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);
    int fork_server_fd = launch_fork_server();

    struct Button buttons[] = {
        (struct Button){.type = LOAD, .x = 10},
        (struct Button){.type = SAVE},
        (struct Button){.type = LAUNCH},
        (struct Button){.type = UP},
        (struct Button){.type = DOWN},
        (struct Button){.type = INSERT},
        (struct Button){.type = DELETE},
        (struct Button){.type = RANDOM}
    };
    const int buttonsLen = sizeof(buttons) / sizeof(buttons[0]);
    struct Button selectAlgoButtons[ALGO_LEN];

    Display *display = XOpenDisplay(NULL);
    if(!display) {
        fprintf(stderr, "Failed to open display\n");
        return 1;
    }
    int blackColor = BlackPixel(display, DefaultScreen(display));
    int whiteColor = WhitePixel(display, DefaultScreen(display));
    XVisualInfo vInfo;
    if(!XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &vInfo)) {
        fprintf(stderr, "No matching visual found\n");
        return 1;
    }
    Colormap colormap = XCreateColormap(display, DefaultRootWindow(display), vInfo.visual, AllocNone);
    if(!colormap) {
        fprintf(stderr, "Failed to create colormap\n");
        return 1;
    }
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

    int windowHeight = 400;
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 400, windowHeight, 0, blackColor, lightGrayColor);
    XStoreName(display, window, "XSort");
    XSelectInput(display, window, StructureNotifyMask);
    XMapWindow(display, window);

    GC lineGC = XCreateGC(display, window, 0, NULL);
    GC borderGC = XCreateGC(display, window, 0, NULL);
    GC fillGC = XCreateGC(display, window, 0, NULL);
    GC textGC = XCreateGC(display, window, 0, NULL);
    GC textAreaGC = XCreateGC(display, window, 0, NULL);
    GC selectedTextGC = XCreateGC(display, window, 0, NULL);
    if(!lineGC || !borderGC || !fillGC || !textGC || !textAreaGC || !selectedTextGC) {
        fprintf(stderr, "Failed to create graphics context\n");
        return 1;
    }
    XSetForeground(display, lineGC, blackColor);
    XSetForeground(display, borderGC, blackColor);
    XSetForeground(display, fillGC, grayColor);
    XSetForeground(display, textGC, blackColor);
    XSetForeground(display, textAreaGC, whiteColor);
    XSetForeground(display, selectedTextGC, blueColor);
    XSetFont(display, lineGC, font->fid);
    XSetFont(display, borderGC, font->fid);

    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
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
    int algoSelection = 0;

    for(;;) {
        bool changed = false;
        XEvent e;
        XNextEvent(display, &e);
        if(e.type == ClientMessage && (Atom)e.xclient.data.l[0] == WM_DELETE_WINDOW) {
            break;
        }
        if(e.type == ConfigureNotify && e.xconfigure.height != windowHeight) {
            windowHeight = e.xconfigure.height;
            changed = true;
        }
        if(e.type == KeyPress) {
            KeySym keysym = XLookupKeysym(&e.xkey, 0);
            if(keysym == XK_Escape) {
                break;
            }
            if(keysym == XK_BackSpace) {
                inputNr /= 10;
                changed = true;
            } else if(keysym == XK_minus || keysym == XK_KP_Subtract) {
                if(inputNr == INT64_MIN) {
                    inputNr = INT64_MAX;
                } else {
                    inputNr = -inputNr;
                }
                changed = true;
            } else {
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
                    changed = true;
                }
            }
        }
        if(e.type == ButtonPress) {
            if(e.xbutton.button != Button1) {
                continue;
            }
            bool found = false;
            enum ButtonType type;
            int x = e.xbutton.x;
            int y = e.xbutton.y;
            for(int i = 0;i < buttonsLen; i++) {
                if(in_bounds(x, y, &buttons[i])) {
                    type = buttons[i].type;
                    found = true;
                    break;
                }
            }
            for(int i = 0;i < ALGO_LEN; i++) {
                if(in_bounds(x, y, &selectAlgoButtons[i])) {
                    type = ALGO_SELECT;
                    algoSelection = i;
                    found = true;
                    break;
                }
            }
            if(!found) {
                continue;
            }
            changed = true;
            switch(type) {
                case ALGO_SELECT:
                    // nothing to do, algoSelection was updated in the loop
                    break;
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
                    write_int(fork_server_fd, algoSelection);
                    write_int(fork_server_fd, bufLen);
                    write_(fork_server_fd, (char*)buf, bufLen * sizeof(int64_t));
                    break;
                case UP:
                    bufSelection = i_max(0, bufSelection - 1);
                    break;
                case DOWN:
                    bufSelection = i_min(bufSelection + 1, bufLen);
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
        }
        if(changed || e.type == Expose) {
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
            const int availableSpace = i_max(1, (windowHeight - y) / (font->ascent + font->descent + 5) - 2);
            int numStart = bufSelection - availableSpace / 2;
            int numEnd = bufSelection + (availableSpace + 1) / 2;
            if(numStart < 0) {
                numEnd = i_min(numEnd + (-numStart), bufLen);
                numStart = 0;
            } else if(numEnd > bufLen) {
                numStart = i_max(numStart - (numEnd - bufLen), 0);
                numEnd = bufLen;
            }
            if(numStart > 0) {
                XDrawString(display, window, textGC, 10, y, dots, strlen(dots));
            }
            selectAlgoButtons[0].y = y + (font->ascent + font->descent) / 2;
            y += font->ascent + font->descent + 5;
            int arrowX;
            int arrowY;
            assert(numStart >= 0 && numEnd <= bufLen);
            for(int i = numStart; i < numEnd && i < bufLen; i++) {
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
            const char *bigNr = "99999999999999999999999";
            int maxPossibleNumWidth = XTextWidth(font, bigNr, strlen(bigNr));
            selectAlgoButtons[0].x = buttons[0].x + maxPossibleNumWidth + 20;
            for(int i = 0; i < ALGO_LEN; i++) {
                drawRadioButton(algo_names[i], selectAlgoButtons[i].x, selectAlgoButtons[i].y, display, window, borderGC, textAreaGC, textGC, font, &selectAlgoButtons[i].width, &selectAlgoButtons[i].height, i == algoSelection);
                if(i != ALGO_LEN - 1) {
                    selectAlgoButtons[i + 1].x = selectAlgoButtons[i].x;
                    selectAlgoButtons[i + 1].y = selectAlgoButtons[i].y + selectAlgoButtons[i].height + 10;
                }
            }
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
