#include "../include/keyboard.h"
#include "../include/idt.h"
#include "../include/pic.h"
#include "../include/ports.h"
#include "../include/vga.h"

/* PS/2 键盘数据端口与状态端口 */
#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* 状态寄存器 bit5：数据来自辅助端口（鼠标）。
 * 键盘和鼠标共用 0x60，读到鼠标数据时必须留给 IRQ12 处理。 */
#define STATUS_AUX_DATA 0x20

/* 环形缓冲区大小（必须是 2 的幂，便于用位与取模） */
#define KBD_BUFFER_SIZE 64
#define KBD_BUFFER_MASK (KBD_BUFFER_SIZE - 1)

static volatile uint16_t kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static volatile bool shift_pressed = false;
static volatile bool caps_lock     = false;
static volatile bool extended      = false;

/* 普通按键映射表（Scancode Set 1，按下时的通码） */
static const char keymap_normal[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  0,   ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* F1-F10 */
    0, 0,                            /* NumLock, ScrollLock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,    /* 小键盘 */
    0, 0, 0, 0, 0
};

/* Shift 按下时的映射表 */
static const char keymap_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*',  0,   ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
};

/* 把按键放进缓冲区；满了就丢弃（不在中断里自旋） */
static void kbd_push(uint16_t c) {
    uint32_t next = (kbd_head + 1) & KBD_BUFFER_MASK;

    if (next == kbd_tail) {
        return;
    }
    kbd_buffer[kbd_head] = c;
    kbd_head = next;
}

/* IRQ1 处理函数 */
static void keyboard_callback(registers_t* regs) {
    uint8_t scancode;
    bool released;
    uint8_t code;

    (void)regs;

    /* 如果这次是鼠标的数据，别碰它，留给 IRQ12 的处理器 */
    if (inb(KEYBOARD_STATUS_PORT) & STATUS_AUX_DATA) {
        pic_send_eoi(1);
        return;
    }

    scancode = inb(KEYBOARD_DATA_PORT);

    /* 0xE0 前缀：下一个字节是扩展键 */
    if (scancode == 0xE0) {
        extended = true;
        pic_send_eoi(1);
        return;
    }

    released = (scancode & 0x80) != 0;
    code     = scancode & 0x7F;

    if (extended) {
        extended = false;
        if (!released) {
            switch (code) {
                case 0x48: kbd_push(KEY_UP);       break;
                case 0x50: kbd_push(KEY_DOWN);     break;
                case 0x4B: kbd_push(KEY_LEFT);     break;
                case 0x4D: kbd_push(KEY_RIGHT);    break;
                case 0x49: kbd_push(KEY_PAGEUP);   break;
                case 0x51: kbd_push(KEY_PAGEDOWN); break;
                case 0x47: kbd_push(KEY_HOME);     break;
                case 0x4F: kbd_push(KEY_END);      break;
                case 0x53: kbd_push(KEY_DELETE);   break;
                default: break;
            }
        }
        pic_send_eoi(1);
        return;
    }

    /* 跟踪 Shift */
    if (code == 0x2A || code == 0x36) {
        shift_pressed = !released;
        pic_send_eoi(1);
        return;
    }

    /* CapsLock 在按下瞬间翻转一次 */
    if (code == 0x3A && !released) {
        caps_lock = !caps_lock;
        pic_send_eoi(1);
        return;
    }

    if (!released) {
        char c = shift_pressed ? keymap_shift[code] : keymap_normal[code];

        /* CapsLock 只影响字母 */
        if (caps_lock && c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        } else if (caps_lock && c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }

        if (c != 0) {
            kbd_push(c);
        }
    }

    pic_send_eoi(1);
}

void keyboard_init(void) {
    kbd_head = 0;
    kbd_tail = 0;
    shift_pressed = false;
    caps_lock     = false;
    extended      = false;

    register_interrupt_handler(IRQ1, keyboard_callback);
    pic_clear_mask(1);
}

bool keyboard_has_char(void) {
    return kbd_head != kbd_tail;
}

int keyboard_getchar(void) {
    for (;;) {
        uint16_t c;

        while (kbd_head == kbd_tail) {
            cpu_halt();
        }

        c = kbd_buffer[kbd_tail];
        kbd_tail = (kbd_tail + 1) & KBD_BUFFER_MASK;

        /* 翻历史相关的按键就地处理掉，不交给调用者。
         * 但全屏程序（切到备用屏幕的那种）需要拿到所有按键，
         * 这时候不能再截胡，否则编辑器用不了 PageUp / Home 之类的键。
         */
        if (!vga_alternate_active()) {
            switch (c) {
                case KEY_PAGEUP:
                    vga_scrollback(VGA_HEIGHT);
                    continue;
                case KEY_PAGEDOWN:
                    vga_scrollback(-VGA_HEIGHT);
                    continue;
                case KEY_HOME:
                    vga_scrollback_top();
                    continue;
                case KEY_END:
                    vga_scrollback_reset();
                    continue;
                default:
                    break;
            }
        }

        return (int)c;
    }
}

bool keyboard_shift_pressed(void) {
    return shift_pressed;
}
