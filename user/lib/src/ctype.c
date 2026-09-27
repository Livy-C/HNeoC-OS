#include <ctype.h>

/* 全部按 C locale 处理：只认 ASCII */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }

int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* 可打印字符：空格到 ~ */
int isprint(int c) {
    return c >= 32 && c < 127;
}

int iscntrl(int c) {
    return (c >= 0 && c < 32) || c == 127;
}

int ispunct(int c) {
    return isprint(c) && c != ' ' && !isalnum(c);
}

int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
