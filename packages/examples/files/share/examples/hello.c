/* hello.c - example source for the on-OS hncc C compiler
 *
 * This file is not part of the OS build. It ships in
 * /share/examples/hello.c so that the machine itself can show what a
 * HNeoC program looks like, and so that the on-OS C compiler (hncc)
 * has something small to chew on:
 *
 *     hncc /share/examples/hello.c -o /bin/exhello
 *
 * Only a modest subset of C is used on purpose, because that is what a
 * teaching compiler can handle: int, char, arrays, pointers, functions,
 * if / while / for, return and string literals. No structs, no
 * typedef, no variadic functions, no floating point.
 */

#include <hneoc.h>

/* How many characters are in s, not counting the terminating '\0'. */
int string_length(char* s) {
    int n = 0;

    while (s[n] != '\0') {
        n = n + 1;
    }
    return n;
}

/* Copy src into dst. Returns how many characters were copied. */
int string_copy(char* dst, char* src) {
    int i = 0;

    while (src[i] != '\0') {
        dst[i] = src[i];
        i = i + 1;
    }
    dst[i] = '\0';
    return i;
}

/* puts would be shorter, but a program that walks a string one
 * character at a time is the point of the example. */
void print_chars(char* s) {
    int i = 0;

    while (s[i] != '\0') {
        putchar(s[i]);
        i = i + 1;
    }
    putchar('\n');
}

int main(void) {
    char greeting[32];
    char* who = "world";
    int n = 0;
    int i = 0;

    /* Build "hello world!" in the buffer, one piece at a time. */
    n = string_copy(greeting, "hello ");
    n = n + string_copy(greeting + n, who);
    n = n + string_copy(greeting + n, "!");

    puts("--- an example program ---");
    print_chars(greeting);

    puts("characters copied:");
    put_int(n);
    putchar('\n');

    /* One dash per character, so the two counts can be eyeballed. */
    for (i = 0; i < n; i = i + 1) {
        putchar('-');
    }
    putchar('\n');

    puts("string_length agrees:");
    put_int(string_length(greeting));
    putchar('\n');

    return 0;
}
