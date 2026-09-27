/* hi - the greeting program shipped by the "hello" package
 *
 * This is an ordinary HNeoC user program: it is compiled by build.ps1
 * into build/packages/hello/hi.lxe, packed into hello-1.0.hnpkg by
 * tools/mkhnpkg.ps1, and installed into /bin/hi.lxe by hpm.
 *
 * Everything it prints is ASCII on purpose - the VGA font is CP437
 * and has no CJK glyphs, so Chinese would come out as garbage.
 */

#include <hneoc.h>

int main(void) {
    unsigned int t = uptime();

    term_set_fg(TERM_LIGHT_GREEN);
    puts("hello 1.0 - installed with hpm");
    term_reset();

    puts("  package   : hello");
    puts("  file      : /bin/hi.lxe");
    fputs("  uptime    : ", STDOUT_FILENO);
    put_uint(t);
    puts(" s since boot");

    if (t == 0) {
        puts("  (the machine booted less than a second ago)");
    } else {
        puts("  nice to meet you - try 'hpm files hello'");
    }

    return 0;
}
