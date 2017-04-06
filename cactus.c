// to compile, run 'cc cactus.c -o cactus'
// to run cactus, './cactus'

/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> // turn off echoing
#include <unistd.h> // need for input


/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/
struct termios orig_termios; // original terminal attributes

/*** terminal ***/
// error handling
void die(const char *s) {
    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if(tcgetattr(STDIN_FILENO, &orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode); // callsdisableRawMode automatically when program exits

    struct termios raw = orig_termios;

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

/*** init ***/
int main() {
    enableRawMode();

    while (1) {
        char c = '\0';
        if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
            die("read");
        // display keypresses
        // isprint() tests whether a character is printable
        if (isprint(c)) {
            printf("%d ('%c')\r\n", c, c);
        } else {
            printf("%d\r\n", c);
        }

        // if input is 'q', exit
        if (c == CTRL_KEY('q')) break;
    }

    return 0;
}
