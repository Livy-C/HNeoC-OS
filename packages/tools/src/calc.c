/* calc - the integer calculator shipped by the "tools" package
 *
 * Line based and deliberately simple: characters are read one at a time
 * with getchar() until '\n', then the expression is evaluated strictly
 * left to right - no operator precedence, no parentheses. That matches
 * how a beginner reads "2 + 3 * 4" on a cheap pocket calculator, and it
 * keeps the parser at about sixty lines.
 *
 *   calc> 2+3*4
 *   = 20
 *   calc> 10 / 0
 *   error: division by zero
 *   calc> q
 *   bye
 *
 * Type q or quit to leave. The kernel does not echo what getchar()
 * returns (the shell echoes its own input), so this program echoes the
 * characters it reads and handles backspace itself.
 *
 * ASCII only on purpose: the VGA font is CP437 and has no CJK glyphs.
 */

#include <hneoc.h>

#define LINE_MAX 64

/* Read one line into buf, echoing as we go.
 * Returns the number of characters stored (without the newline), or -1
 * if the line did not fit. An overlong line is still consumed up to the
 * newline, so the next read starts on a fresh line.
 */
static int read_line(char* buf, int max) {
    int n = 0;
    int overflow = 0;

    for (;;) {
        int c = getchar();

        if (c == '\n') {
            putchar('\n');
            break;
        }

        if (c == '\b') {
            if (n > 0) {
                n--;
                /* Move back, overwrite with a space, move back again.
                 * The kernel's '\b' only moves the cursor. */
                fputs("\b \b", STDOUT_FILENO);
            }
            continue;
        }

        /* Ignore anything that is not a printable ASCII character:
         * tabs, ESC and any stray byte we could not evaluate anyway. */
        if (c < ' ' || c > '~') {
            continue;
        }

        if (n >= max - 1) {
            overflow = 1;
            continue;
        }

        buf[n++] = (char)c;
        putchar(c);
    }

    buf[n] = '\0';
    return overflow ? -1 : n;
}

/* Skip blanks, return the index of the first interesting character. */
static int skip_spaces(const char* s, int i) {
    while (s[i] == ' ' || s[i] == '\t') { i++; }
    return i;
}

/* Parse a decimal integer starting at *pi (spaces allowed in front).
 * Returns 1 and advances *pi on success, 0 if there is no number. */
static int parse_int(const char* s, int* pi, int* out) {
    int i = skip_spaces(s, *pi);
    int neg = 0;
    int digits = 0;
    int v = 0;

    if (s[i] == '-') { neg = 1; i++; }
    else if (s[i] == '+') { i++; }

    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
        digits = 1;
    }

    if (!digits) { return 0; }

    *pi = i;
    *out = neg ? -v : v;
    return 1;
}

/* Evaluate "<number> <op> <number> ...", left to right.
 * Returns 1 and stores the result, or 0 with *err set to a message. */
static int evaluate(const char* s, int* result, const char** err) {
    int i = 0;
    int acc = 0;
    int v = 0;

    if (!parse_int(s, &i, &acc)) {
        *err = "expected a number";
        return 0;
    }

    for (;;) {
        char op;

        i = skip_spaces(s, i);
        if (s[i] == '\0') {
            *result = acc;
            return 1;
        }

        op = s[i];
        if (op != '+' && op != '-' && op != '*' && op != '/') {
            *err = "unknown operator, use + - * /";
            return 0;
        }
        i++;

        if (!parse_int(s, &i, &v)) {
            *err = "expected a number after the operator";
            return 0;
        }

        if (op == '+') {
            acc = acc + v;
        } else if (op == '-') {
            acc = acc - v;
        } else if (op == '*') {
            acc = acc * v;
        } else {
            if (v == 0) {
                *err = "division by zero";
                return 0;
            }
            /* -2147483648 / -1 traps the CPU on x86; report it instead
             * of letting the divider fault kill the program. */
            if (v == -1 && acc == (-2147483647 - 1)) {
                *err = "result does not fit in 32 bits";
                return 0;
            }
            acc = acc / v;
        }
    }
}

int main(void) {
    char line[LINE_MAX];

    term_set_fg(TERM_LIGHT_CYAN);
    puts("calc - integer calculator (tools 1.0)");
    term_reset();
    puts("  expressions are evaluated left to right, no parentheses");
    puts("  example:  12 + 30 / 6      gives 7, not 17");
    puts("  q or quit to leave");
    puts("");

    for (;;) {
        int n;
        int i;
        int result = 0;
        const char* err = "?";

        term_set_fg(TERM_YELLOW);
        fputs("calc> ", STDOUT_FILENO);
        term_reset();

        n = read_line(line, (int)sizeof(line));
        if (n < 0) {
            puts("error: line too long, it must fit in 63 characters");
            continue;
        }

        i = skip_spaces(line, 0);

        if (line[i] == '\0') {
            continue;                       /* empty line: just prompt again */
        }
        if (str_eq(&line[i], "q") || str_eq(&line[i], "quit")) {
            break;
        }

        if (evaluate(&line[i], &result, &err)) {
            fputs("= ", STDOUT_FILENO);
            put_int(result);
            putchar('\n');
        } else {
            fputs("error: ", STDOUT_FILENO);
            puts(err);
        }
    }

    puts("bye");
    return 0;
}
