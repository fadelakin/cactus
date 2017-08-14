// to run cactus, 'make all', then './cactus'

/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h> // turn off echoing
#include <time.h>
#include <unistd.h> // need for input


/*** defines ***/

#define CACTUS_VERSION "0.0.1"
#define CACTUS_TAB_STOP 8
#define CACTUS_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
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

// enum containing possible values that hl can contain
enum editorHighlight {
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_MATCH
};

/*** data ***/

// editor row
typedef struct erow {
    int size;
    int rsize; // render size
    char *chars;
    char *render;
    unsigned char *hl; // array for highlighting each line in an array
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
    int dirty; // marker for bugger if it has been modified since opening or saving the file.
    char *filename; // filename for status bar
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios; // original terminal attributes
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);

// implicit declaration of function 'ioctl' is invalid in C99
int ioctl(int fd, unsigned long request, ...);

void editorRefreshScreen();

char *editorPrompt(char *prompt, void (*callback)(char *, int));


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

/*** syntax highlighting ***/

int is_seperator(int c) {
    // take a character and return true if it's a seperator character
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    // set all characters to HL_NORMAL by default
    memset(row->hl, HL_NORMAL, row->rsize);

    int prevSep = 1; // keep track if the previous character is a seperator

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if ((isdigit(c) && (prevSep || prev_hl == HL_NUMBER)) ||
            (c == '.' && prev_hl == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            i++;
            prevSep = 0;
            continue;
        }

        prevSep = is_seperator(c);
        i++;
    }
}

// map values in hl to actual ANSI color codes we want to draw them with
int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_NUMBER: return 31; // return red for numbers
        case HL_MATCH: return 34; // return blue for search result
        default: return 37; // return white for anything else
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

// convert render index to chars index before assigning it
int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++) {
        if(row->chars[cx] == '\t') {
            cur_rx += (CACTUS_TAB_STOP - 1) - (cur_rx % CACTUS_TAB_STOP);
        }
        cur_rx++;

        if(cur_rx > rx) return cx;
    }
    return cx; // just in case caller provided a rx that is out of range
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

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numRows) return;

    // allocate space for a new erow and then copy the given string to a new erow at the end of E.row array
    E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numRows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    editorUpdateRow(&E.row[at]);

    E.numRows++;
    E.dirty++;
}

// free memory owned by the erow
void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numRows) return;
    editorFreeRow(&E.row[at]);
    // overwrite the deleted row struct with the rest of the rows that come after it
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numRows - at - 1));
    E.numRows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    // validate the index we want to insert the character into
    if(at < 0 || at > row->size) at = row->size;
    // allocate one more byte for the chars of the erow
    // add 2 because we need to also make room for the null byte
    row->chars = realloc(row->chars, row->size + 2);
    // make room for the new character
    memmove(&row->chars[at + 1], &row->chars[at], row->size -at + 1);
    // increase the size of the chars array
    row->size++;
    // assign the character to its position in the array
    row->chars[at] = c;
    // update render and rsize with the new row content
    editorUpdateRow(row);
    E.dirty++;
}

// append a string to the end of a row
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

// deletes a character in an erow
void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    // overwrite the deleted character with the characters that come after it2
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

// take a character and use editorRowInsertChar() to insert that character
// into a position the cursor is at
void editorInsertChar(int c) {
    if(E.cy == E.numRows) {
        // cursor is on the tilde line after the end of the file
        // so append a new row to the file before inserting character
        editorInsertRow(E.numRows, "", 0);
    }

    // move cursor forward so next character the user inserts
    // will go after the character just inserted
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

// handle 'enter' keypress
void editorInsertNewLine() {
    if(E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

// delete the character that is to the left of the cursor
void editorDelChar() {
    // if cursor is past the end of the file, nothing to delete
    if(E.cy == E.numRows) return;
    if(E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    // if there is a character to the left of the cursor, delete and move cursor one to the left
    if(E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

// convert array of erow structs into a string strings that writes out to a file
char *editorRowsToString(int *buffLen) {
    int totLen = 0;
    int j;
    for(j = 0; j < E.numRows; j++) {
        // add up the lengths of each row of text
        // add 1 to each one for the newline char we add to the end of each line
        totLen += E.row[j].size + 1;
    }
    // save the total length into buffLen
    *buffLen = totLen;

    char *buf = malloc(totLen);
    char *p = buf;
    // loop through the rows
    for(j = 0; j < E.numRows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n'; // append newline character after each row
        p++;
    }

    return buf;
}

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
        editorInsertRow(E.numRows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

// write the string returned by editorRowsToString() to disk
void editorSave() {
    // if a new file, filename is null
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    // create a new file if it doesn't already exist and open it for reading and writing
    // 0644 is the standard permission for text files
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

    if(fd != -1) {
        if(ftruncate(fd, len) != -1) {
            if(write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk.", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key) {
    static int lastMatch = -1; // index of the row the last match was on
    static int direction = 1; // direction of search; 1 - forward; -1 - backward

    // restore text color after search
    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    // check if user pressed 'enter' or 'escape', if so, leave search mode
    if (key == '\r' || key == '\x1b') {
        lastMatch = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        lastMatch = -1;
        direction = 1;
    }

    if (lastMatch == -1) direction = 1;
    int current = lastMatch; // index of the current row wer are searching

    // loop through all the rows of the file
    int i;
    for(i = 0; i < E.numRows; i++) {
        current += direction;
        if (current == -1) current = E.numRows - 1;
        else if (current == E.numRows) current = 0;

        erow *row = &E.row[current];
        // check if query is a substring of the current row
        char *match = strstr(row->render, query);
        if(match) {
            lastMatch = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowOff = E.numRows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.colOff;
    int saved_rowoff = E.rowOff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if(query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.colOff = saved_coloff;
        E.rowOff = saved_rowoff;
    }
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

            // attempt to highlight numbers by coloring each digit char red
            char *c  = &E.row[fileRow].render[E.colOff];
            unsigned char *hl = &E.row[fileRow].hl[E.colOff];
            int current_color = -1; // default text color
            int j;
            for (j = 0; j < len; j++) {
                if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5); // default text color
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int cLen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, cLen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }

        abAppend(ab, "\x1b[K", 3);
        // make room for status bar
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numRows,
        E.dirty ? "(modified)" : "");
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
    abAppend(ab, "\r\n", 2); // display our status message
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3); // clear the message bar
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screenCols) msglen = E.screenCols;
    if(msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // clear screen using VT100 escape sequences
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3); // position cursor

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowOff) + 1, (E.rx - E.colOff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    // create our own printf() style function
    // and store the resulting string in E.statusmsg
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);

    // set E.statusmsg_time to the current time
    E.statusmsg_time = time(NULL);
}

/*** input ***/

// display a prompt in the status bar & let the user input a line of text after the prompt
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufSize = 128;
    char *buf = malloc(bufSize); // store user input

    size_t bufLen = 0;
    buf[0] = '\0';

    // repeatedly set the status message, refresh the screen and wait for a keypress to handle
    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (bufLen != 0) buf[--bufLen] = '\0';
        } else if (c  == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (bufLen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (bufLen == bufSize - 1) {
                bufSize *= 2;
                buf = realloc(buf, bufSize);
            }
            buf[bufLen++] = c;
            buf[bufLen] = '\0';
        }

        if(callback) callback(buf, c);
    }
}

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
    static int quitTimes = CACTUS_QUIT_TIMES;

    int c = editorReadKey();

    switch(c) {
        // ignore enter key
        case '\r':
            editorInsertNewLine();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quitTimes > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quitTimes);
                quitTimes--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if(E.cy < E.numRows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        // handle backspace or delete key
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
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

        // do nothing with the ctrl-l and escape keys
        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quitTimes = CACTUS_QUIT_TIMES;
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
    E.dirty = 0; // initialize dirty state
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenRows, &E.screenCols) == -1) die("getWindowSize");
    E.screenRows -= 2;
}

int main(int argc, char* argv[]) {
    enableRawMode();
    initEditor();
    if(argc >= 2) {
        editorOpen(argv[1]);
    }

    // set initial status message
    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
