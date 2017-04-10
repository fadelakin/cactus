// to compile, run 'cc cactus.c -o cactus'
// to run cactus, './cactus'

/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h> // turn off echoing
#include <unistd.h> // need for input


/*** defines ***/

#define CACTUS_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

// editor row
typedef struct erow {
    int size;
    char *chars;
} erow;

// contain editor state
struct editorConfig {
    int cx, cy;
    int screenRows;
    int screenCols;
    int numRows;
    erow row;
    struct termios orig_termios; // original terminal attributes
};

struct editorConfig E;

// implicit declaration of function 'ioctl' is invalid in C99
int ioctl(int fd, unsigned long request, ...);

/*** terminal ***/

// error handling
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode); // callsdisableRawMode automatically when program exits

    struct termios raw = E.orig_termios;

    raw.c_lflag &= ~(OPOST); // turn off all output processing
    // CS8 is a bit mask with multiple bits.
    // sets the character size (CS) to 8 bits per byte
    raw.c_lflag |= (CS8);
    // disable Ctrl-S/Q and fix Ctrl-M
    raw.c_lflag &= ~(ICRNL | IXON);

    // turn off canonical mode and echoing and Ctrl-C/Z/V signals
    // program exits immediately when a q is detected
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // turn off break conditions, parity checking, 8th bit of each input byte to be stripped
    raw.c_lflag &= ~(BRKINT | INPCK | ISTRIP);


    // set timeout for read()
    raw.c_cc[VMIN] = 0; // set min num of bytes of input needed before read() can return
    raw.c_cc[VTIME] = 1; // set maximum amount of time to wait before read() returns

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

// wait for one keypress and return it
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // read arrow keys
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if(seq[1] >= '0' && seq[1] <= '9') {
                if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if(seq[2] == '~') {
                    switch(seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                // return corresponding wasd character for arrow key escape sequence
                switch(seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == '0') {
            switch(seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

// ioctl() isn't guaranteed to be able to request the window size on all system
// so fallback method of getting window size
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // send two escape sequences one after the other to move cursor
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** file i/o ***/

// open and read file from disk
void editorOpen(char *filename) {
    // read and display the first line of the file
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    ssize_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp);
    if(linelen != -1) {
        while(linelen > 0 && (line[linelen -1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        E.row.size = linelen;
        E.row.chars = malloc(linelen + 1);
        memcpy(E.row.chars, line, linelen);
        E.row.chars[linelen] = '\0';
        E.numRows = 1;
    }
    free(line);
    fclose(fp);
}

/***
append buffer
- do one big write to make sure the whole screen updates at once
***/

struct abuf {
    char *b;
    int len;
};

# define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    // allocate enough memory to hold new string
    // realloc() will either extend the size of the block of memory we already have allocated or free it
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

// destructor that deallocates the dynamic memory used by an abuf
void abFree(struct abuf *ab) {
    // free the current block of memory and allocating a new block of memory somewhere else.
    free(ab->b);
}

/*** output ***/

// handle drawing each row of the buffer of text being edited
// drawing 24 rows for now
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenRows; y++) {
        // check if we are currently drawing a row that is part of the text buffer
        // or a row that comes after the end of the text buffer
        if(y >= E.numRows) {
            if(E.numRows == 0 && y == E.screenRows / 3) {
                char welcome[80];
                int welcomeLen = snprintf(welcome, sizeof(welcome),
                "Cactus -- version %s",
                CACTUS_VERSION);
                if(welcomeLen > E.screenCols) welcomeLen = E.screenCols;
                // center welcome message
                int padding = (E.screenCols - welcomeLen) / 2;
                if(padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomeLen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row.size;
            if(len > E.screenCols) len = E.screenCols;
            abAppend(ab, E.row.chars, len);
        }

        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenRows -1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    // clear screen using VT100 escape sequences
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3); // position cursor

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

// move cursor with w,a,s,d keys
// w - up, a - left, s - down, d - right
void editorMoveCursor(int key) {
    switch(key) {
        case ARROW_LEFT:
            if(E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if(E.cx != E.screenCols - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if(E.cy != E.screenRows - 1) {
                E.cy++;
            }
            break;
    }
}

// wait for keypress and handle it
void editorProcessKeypress() {
    int c = editorReadKey();

    switch(c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screenCols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenRows;
                while(times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*** init ***/

// initialize all fields in the E struct
void initEditor() {
    E.cx = 0; // horizontal coordinate of cursor
    E.cy = 0; // vertical coordinate of cursor
    E.numRows = 0;

    if (getWindowSize(&E.screenRows, &E.screenCols) == -1) die("getWindowSize");
}

int main(int argc, char* argv[]) {
    enableRawMode();
    initEditor();
    if(argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
