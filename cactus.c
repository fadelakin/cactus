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
#define CACTUS_TAB_STOP 8

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
    int rsize; // render size
    char *chars;
    char *render;
} erow;

// contain editor state
struct editorConfig {
    int cx, cy;
    int rx;  // index into the render field
    int rowOff; // row offset: what row of the file the user is currently scrolled to
    int colOff; // column offset
    int screenRows;
    int screenCols;
    int numRows;
    erow *row; // an array of erow structs to store multiple lines
    char *filename; // filename for status bar
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

/*** row operations ***/

// convert a chars index into a render index
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    // loop through all the characters to the left of cx and figure out how many spaces each tab takes up
    for(j = 0; j < cx; j++) {
        if(row->chars[j] == '\t') {
            // find how many columns we are to the right of the last tab stop
            // then subtract that from CACTUS_TAB_STOP - 1
            // to find out how many columns we are to the left of the next tab stop
            // then add that amount to rx to get just to the left of the next tab stop
            rx += (CACTUS_TAB_STOP - 1) - (rx % CACTUS_TAB_STOP);
        }
        rx++; // get us right on the next tab stop
    }
    return rx;
}

// use the chars string of an erow to fill the contents of the render string
void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    // render tabs as multiple space characters
    for(j = 0; j < row->size; j++) {
        if(row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(CACTUS_TAB_STOP - 1) + 1);

    int index = 0;
    for(j = 0; j < row->size; j++) {
        if(row->chars[j] == '\t') {
            row->render[index++] = ' ';
            // max # of chars for each tab is 8
            while(index % CACTUS_TAB_STOP != 0) row->render[index++] = ' ';
        } else {
            row->render[index++] = row->chars[j];
        }
    }
    row->render[index] = '\0';
    row->rsize = index;
}

void editorAppendRow(char *s, size_t len) {
    // allocate space for a new erow and then copy the given string to a new erow at the end of E.row array
    E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));

    int at = E.numRows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numRows++;
}

/*** file i/o ***/

// open and read file from disk
void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    // read and display the first line of the file
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    ssize_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp);
    // read entire file into E.row
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while(linelen > 0 && (line[linelen -1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorAppendRow(line, linelen);
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

// check if the cursor has moved outside of the visible window
// if so, adjust E.rowOff so the cursor is just inside the visible window
void editorScroll() {
    E.rx = 0;
    if(E.cy < E.numRows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // if the cursor is above the visible window, scroll up to where the cursor is
    if(E.cy < E.rowOff) {
        E.rowOff = E.cy;
    }

    // if the cursor is past the bottom of the visible window, scroll down to where the cursor is
    // but not past the end of the file
    if(E.cy >= E.rowOff + E.screenRows) {
        E.rowOff = E.cy - E.screenRows + 1;
    }

    // horizontal scrolling
    if(E.rx < E.colOff) {
        E.colOff = E.rx;
    }

    if(E.rx >= E.colOff + E.screenCols) {
        E.colOff = E.rx - E.screenCols + 1;
    }
}

// handle drawing each row of the buffer of text being edited
// drawing 24 rows for now
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenRows; y++) {
        int fileRow = y + E.rowOff;
        // check if we are currently drawing a row that is part of the text buffer
        // or a row that comes after the end of the text buffer
        if(fileRow >= E.numRows) {
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
            int len = E.row[fileRow].rsize - E.colOff;
            if(len < 0) len = 0;
            if(len > E.screenCols) len = E.screenCols;
            abAppend(ab, &E.row[fileRow].render[E.colOff], len);
        }

        abAppend(ab, "\x1b[K", 3);
        // make room for status bar
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[1;4;7m", 4); // bold and invert colors in status bar
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
        E.filename ? E.filename : "[No Name]", E.numRows);
    int rLen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numRows);
    if(len > E.screenCols) len = E.screenCols;
    abAppend(ab, status, len);
    while (len < E.screenCols) {
        if(E.screenCols - len == rLen) {
            abAppend(ab, rstatus, rLen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3); // go back to default formatting
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // clear screen using VT100 escape sequences
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3); // position cursor

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowOff) + 1, (E.rx - E.colOff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

// move cursor with w,a,s,d keys
// w - up, a - left, s - down, d - right
void editorMoveCursor(int key) {
    // check if cursor is on an actual line
    erow *row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];

    switch(key) {
        case ARROW_LEFT:
            if(E.cx != 0) {
                E.cx--;
            } else if(E.cy > 0) {
                // allow user to press '<-' at beginning of line to move to end of previous line
                E.cy--;
                E.cx = E.row[E.cy].size;
            } else if(row && E.cx == row->size) {
                // allow user to press '->' at the end of line to go to beginning of next line
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_RIGHT:
            if(row && E.cx < row->size) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if(E.cy < E.numRows) {
                E.cy++;
            }
            break;
    }

    // snap cursor to the end of line
    row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];
    int rowLen = row ? row->size : 0;
    if(E.cx > rowLen) {
        E.cx = rowLen;
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
            if(E.cy < E.numRows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if(c == PAGE_UP) {
                    E.cy = E.rowOff;
                } else if(c == PAGE_DOWN) {
                    E.cy = E.rowOff + E.screenRows - 1;
                    if(E.cy > E.numRows) E.cy = E.numRows;
                }

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
    E.rx = 0;
    E.rowOff = 0; // scrolled to the top by default
    E.colOff = 0;
    E.numRows = 0;
    E.row = NULL;
    E.filename = NULL;

    if (getWindowSize(&E.screenRows, &E.screenCols) == -1) die("getWindowSize");
    E.screenRows -= 1;
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
