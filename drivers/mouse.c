#include "../include/mouse.h"
#include "../include/idt.h"
#include "../include/pic.h"
#include "../include/ports.h"
#include "../include/vga.h"

/* 8042 键盘控制器端口 */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

/* 状态寄存器位 */
#define ST_OUTPUT_FULL 0x01   /* 输出缓冲里有数据可读 */
#define ST_INPUT_FULL  0x02   /* 输入缓冲还满着，不能写 */
#define ST_AUX_DATA    0x20   /* 数据来自辅助端口（鼠标）而不是键盘 */

/* 控制器命令 */
#define CMD_READ_CONFIG   0x20
#define CMD_WRITE_CONFIG  0x60
#define CMD_DISABLE_AUX   0xA7
#define CMD_ENABLE_AUX    0xA8
#define CMD_WRITE_TO_AUX  0xD4

/* 鼠标命令 */
#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE       0xF4
#define MOUSE_SET_RATE     0xF3
#define MOUSE_GET_ID       0xF2
#define MOUSE_ACK          0xFA

/* 每个滚轮刻度滚几行 */
#define WHEEL_LINES 3

static bool     present       = false;
static bool     wheel_enabled = false;
static bool     initialized   = false;

static uint8_t  packet[4];
static uint32_t packet_index = 0;
static uint32_t packet_size  = 3;   /* 带滚轮的型号是 4 字节 */

static int32_t  pos_x = 40;
static int32_t  pos_y = 12;
static uint8_t  buttons = 0;

static uint32_t stat_packets = 0;
static uint32_t stat_wheels  = 0;

/* ------------------------------------------------------------
 * 8042 读写辅助
 * ------------------------------------------------------------ */

/* 等到输入缓冲空出来，才能往里写命令或数据 */
static bool wait_input_clear(void) {
    for (uint32_t i = 0; i < 200000; i++) {
        if ((inb(PS2_STATUS) & ST_INPUT_FULL) == 0) {
            return true;
        }
    }
    return false;
}

/* 从输出缓冲读一个字节。
 * want_aux 为 true 时只接受来自鼠标的数据，false 时只接受键盘的。
 * 这一位必须判断：键盘和鼠标共用 0x60 端口，读错了就会把对方的
 * 数据吞掉，两边都会错位。
 */
static bool ps2_read(uint8_t* out, bool want_aux) {
    for (uint32_t i = 0; i < 200000; i++) {
        uint8_t status = inb(PS2_STATUS);

        if ((status & ST_OUTPUT_FULL) == 0) {
            continue;
        }
        if (want_aux && !(status & ST_AUX_DATA)) {
            continue;
        }
        if (!want_aux && (status & ST_AUX_DATA)) {
            continue;
        }

        *out = inb(PS2_DATA);
        return true;
    }
    return false;
}

static void ctrl_write(uint8_t command) {
    wait_input_clear();
    outb(PS2_COMMAND, command);
}

static void aux_write(uint8_t value) {
    ctrl_write(CMD_WRITE_TO_AUX);
    wait_input_clear();
    outb(PS2_DATA, value);
}

/* 给鼠标发命令并等它的 ACK */
static bool aux_command(uint8_t command) {
    uint8_t ack;

    aux_write(command);
    if (!ps2_read(&ack, true)) {
        return false;
    }
    return ack == MOUSE_ACK;
}

/* 给鼠标发一个带参字节（前面已经发过命令并收到 ACK） */
static bool aux_argument(uint8_t value) {
    uint8_t ack;

    aux_write(value);
    if (!ps2_read(&ack, true)) {
        return false;
    }
    return ack == MOUSE_ACK;
}

/* ------------------------------------------------------------
 * 中断处理
 * ------------------------------------------------------------ */
static void mouse_callback(registers_t* regs) {
    (void)regs;

    uint8_t status = inb(PS2_STATUS);

    /* 这一位没置位说明是键盘数据，留给 IRQ1 处理，这里绝对不能读走 */
    if ((status & ST_AUX_DATA) == 0) {
        pic_send_eoi(12);
        return;
    }

    uint8_t data = inb(PS2_DATA);

    /* 包头第一个字节的 bit3 永远是 1。不是的话说明丢了同步，丢掉重来 */
    if (packet_index == 0 && (data & 0x08) == 0) {
        pic_send_eoi(12);
        return;
    }

    packet[packet_index++] = data;

    if (packet_index < packet_size) {
        pic_send_eoi(12);
        return;
    }
    packet_index = 0;
    stat_packets++;

    /* --- 解析 --- */
    {
        uint8_t flags = packet[0];
        int32_t dx;
        int32_t dy;

        /* bit6/bit7 是溢出标志，说明移动太快数据不可信，整包丢掉 */
        if (flags & 0xC0) {
            pic_send_eoi(12);
            return;
        }

        dx = (int32_t)packet[1];
        dy = (int32_t)packet[2];
        if (flags & 0x10) dx -= 256;   /* X 是负数 */
        if (flags & 0x20) dy -= 256;   /* Y 是负数 */

        /* PS/2 的 Y 轴向上为正，屏幕坐标向下为正，所以取反 */
        pos_x += dx;
        pos_y -= dy;

        /* 夹在屏幕范围内，反正现在也还没有画鼠标指针 */
        if (pos_x < 0) pos_x = 0;
        if (pos_x > VGA_WIDTH - 1) pos_x = VGA_WIDTH - 1;
        if (pos_y < 0) pos_y = 0;
        if (pos_y > VGA_HEIGHT - 1) pos_y = VGA_HEIGHT - 1;

        buttons = flags & 0x07;

        /* 第 4 个字节低 4 位是滚轮，4 位有符号数：
         * 1 = 向上滚一格，15 = 向下滚一格
         */
        if (packet_size == 4) {
            int32_t dz = (int32_t)(packet[3] & 0x0F);

            if (dz & 0x08) {
                dz -= 16;
            }

            if (dz != 0) {
                stat_wheels++;
                /* 向上滚 = 看更旧的内容 */
                vga_scrollback((int)(dz * WHEEL_LINES));
            }
        }
    }

    pic_send_eoi(12);
}

/* ------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------ */
void mouse_init(void) {
    uint8_t config;
    uint8_t id;
    uint8_t ack;

    present       = false;
    wheel_enabled = false;
    initialized   = false;
    packet_index  = 0;
    packet_size   = 3;
    stat_packets  = 0;
    stat_wheels   = 0;

    /* 1. 打开辅助 PS/2 端口 */
    ctrl_write(CMD_ENABLE_AUX);

    /* 2. 读配置字节：打开 IRQ12，并解除辅助端口时钟禁用 */
    ctrl_write(CMD_READ_CONFIG);
    if (!ps2_read(&config, false)) {
        return;
    }

    config |= 0x02;    /* bit1 = 允许 IRQ12 */
    config &= 0xDF;    /* bit5 = 0 表示辅助端口时钟正常 */

    ctrl_write(CMD_WRITE_CONFIG);
    if (!wait_input_clear()) {
        return;
    }
    outb(PS2_DATA, config);

    /* 3. 先来一次复位，让鼠标回到确定状态 */
    aux_write(0xFF);
    if (!ps2_read(&ack, true)) {
        return;               /* 端口上没东西，不是鼠标 */
    }
    ps2_read(&ack, true);     /* 0xAA 自检通过 */
    ps2_read(&id, true);      /* 设备 ID */

    /* 4. 设成默认参数 */
    if (!aux_command(MOUSE_SET_DEFAULTS)) {
        return;
    }

    /* 5. IntelliMouse 握手：采样率依次 200 / 100 / 80，
     *    之后读设备 ID，返回 3 或 4 就说明支持滚轮
     */
    if (aux_command(MOUSE_SET_RATE)) {
        aux_argument(200);
        aux_command(MOUSE_SET_RATE);
        aux_argument(100);
        aux_command(MOUSE_SET_RATE);
        aux_argument(80);

        if (aux_command(MOUSE_GET_ID)) {
            if (ps2_read(&id, true)) {
                if (id == 3 || id == 4) {
                    wheel_enabled = true;
                    packet_size   = 4;
                }
            }
        }
    }

    /* 6. 开始上报数据 */
    if (!aux_command(MOUSE_ENABLE)) {
        return;
    }

    present     = true;
    initialized = true;

    /* 7. 打开 IRQ12。
     *    IRQ12 在从片上，从片又是通过主片的 IRQ2 级联的，
     *    所以主片的 IRQ2 也必须一起放开，否则中断永远传不上来。
     */
    register_interrupt_handler(IRQ12, mouse_callback);
    pic_clear_mask(2);
    pic_clear_mask(12);
}

bool mouse_present(void) {
    return present;
}

bool mouse_has_wheel(void) {
    return wheel_enabled;
}

void mouse_get_state(int32_t* x, int32_t* y, uint8_t* btn) {
    if (x)   *x = pos_x;
    if (y)   *y = pos_y;
    if (btn) *btn = buttons;
}

uint32_t mouse_packets(void) {
    return stat_packets;
}

uint32_t mouse_wheel_events(void) {
    return stat_wheels;
}
