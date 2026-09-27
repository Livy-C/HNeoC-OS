/* count.c - example source for the on-OS hncc C compiler
 *
 * The companion to hello.c, and the one that actually does arithmetic:
 * it counts in a while loop, sums a for loop, and prints integers by
 * turning the digits around in a small buffer instead of calling printf.
 *
 *   hncc /share/examples/count.c -o /bin/excount
 *
 * Same modest subset of C as hello.c: int, char, arrays, pointers,
 * functions, if / while / for, return, string literals. The one cast
 * is (char)('0' + digit), which is how you print a number without
 * printf.
 */

#include <hneoc.h>

/* Print a signed integer followed by a newline, without printf. */
void print_int(int value) {
    char digits[12];
    int n = 0;
    int neg = 0;

    if (value < 0) {
        neg = 1;
        value = -value;
    }

    if (value == 0) {
        digits[n] = '0';
        n = n + 1;
    }

    /* The digits come out backwards, least significant first. */
    while (value > 0) {
        digits[n] = (char)('0' + (value % 10));
        n = n + 1;
        value = value / 10;
    }

    if (neg) {
        putchar('-');
    }

    /* Walk the buffer backwards to print the number the right way. */
    while (n > 0) {
        n = n - 1;
        putchar(digits[n]);
    }
    putchar('\n');
}

/* Write a non-negative value into buf, most significant digit first.
 * Returns the number of characters written (the '\0' is not counted).
 */
int int_to_string(char* buf, int value) {
    char tmp[12];
    int n = 0;
    int i = 0;

    if (value == 0) {
        tmp[n] = '0';
        n = n + 1;
    }

    while (value > 0) {
        tmp[n] = (char)('0' + (value % 10));
        n = n + 1;
        value = value / 10;
    }

    while (n > 0) {
        n = n - 1;
        buf[i] = tmp[n];
        i = i + 1;
    }
    buf[i] = '\0';
    return i;
}

int main(void) {
    char buf[16];
    int i = 0;
    int sum = 0;
    int len = 0;

    puts("--- counting example ---");

    /* A while loop that counts up and prints as it goes. */
    puts("counting from 1 to 5:");
    i = 1;
    while (i <= 5) {
        print_int(i);
        i = i + 1;
    }

    /* A for loop that adds its counter to a running total. */
    for (i = 1; i <= 10; i = i + 1) {
        sum = sum + i;
    }
    puts("1 + 2 + ... + 10 =");
    print_int(sum);

    /* The same total, this time as a string in a buffer. */
    puts("as text:");
    len = int_to_string(buf, sum);
    puts(buf);
    puts("characters in that string:");
    print_int(len);

    /* Negative numbers travel through the same code path. */
    puts("and the negative of the total:");
    print_int(0 - sum);

    return 0;
}
