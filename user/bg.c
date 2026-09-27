/* bg - 一个安静的背景任务
 *
 * 不打印任何东西，只是睡 20 秒。spawn 起来之后屏幕上不会有它的输出，
 * 正好可以清楚地看 taskmgr 里的任务列表。
 */

#include "hneoc.h"

int main(void) {
    for (int i = 0; i < 20; i++) {
        sleep_ms(1000);
    }
    return 0;
}
