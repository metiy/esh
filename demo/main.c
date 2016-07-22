#include <esh.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

void esh_print_cb(struct esh * esh, char const * s);
void esh_callback_cb(struct esh * esh, int argc, char ** argv);
static void set_terminal_raw(void);
static void restore_terminal(void);

static struct termios saved_term;

void esh_print_cb(struct esh * esh, char const * s)
{
    (void) esh;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] == '\n') {
            fputc('\r', stdout);
        }
        fputc(s[i], stdout);
    }
}


void esh_callback_cb(struct esh * esh, int argc, char ** argv)
{
    (void) esh;

    if (argc && (!strcmp(argv[0], "exit") || !strcmp(argv[0], "quit"))) {
        exit(0);
    }

    printf("argc     = %d\r\n", argc);

    for (int i = 0; i < argc; ++i) {
        printf("argv[% 2d] = %s\r\n", i, argv[i]);
    }
}


int main(int argc, char ** argv)
{
    (void) argc;
    (void) argv;

    struct esh esh;

    esh_init(&esh);
    esh_register_callback(&esh, esh_callback_cb);
    esh_register_print(&esh, esh_print_cb);

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "%s\n", "esh demo must run on a tty");
        exit(1);
    }

    if (tcgetattr(STDIN_FILENO, &saved_term) < 0) {
        perror("tcgetattr");
        exit(1);
    }

    if (atexit(&restore_terminal)) {
        perror("atexit");
        exit(1);
    }
    set_terminal_raw();

    printf("%s\r\n", "Use 'quit' or 'exit' to quit.");
    esh_rx(&esh, '\n');
    for (;;) {
        int c = getchar();
        if (c > 0 && c <= 255) {
            if (c == '\r') {
                c = '\n';
            }
            esh_rx(&esh, c);
        }
    }

    return 0;
}


void set_terminal_raw(void)
{
    struct termios term = saved_term;

    term.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    term.c_oflag &= ~(OPOST);
    term.c_cflag |= CS8;
    term.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    term.c_cc[VMIN] = 0;    term.c_cc[VTIME] = 8;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term) < 0) {
        perror("tcsetattr");
        exit(1);
    }
}


void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_term);
}
