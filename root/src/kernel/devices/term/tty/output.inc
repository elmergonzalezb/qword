#include <stdint.h>
#include <lib/klib.h>
#include <devices/term/tty/tty.h>
#include <sys/panic.h>
#include <lib/lock.h>
#include <lib/alloc.h>

static int rows;
static int cols;

static uint32_t *fb;
static int fb_height;
static int fb_width;
static int fb_pitch;

static uint8_t *font;
static int font_height;
static int font_width;

static void put_char(int, char);

static void plot_px(int x, int y, uint32_t hex) {
    size_t fb_i = x + (fb_pitch / sizeof(uint32_t)) * y;

    fb[fb_i] = hex;

    return;
}

static void plot_char(char c, int x, int y, uint32_t hex_fg, uint32_t hex_bg) {
    int orig_x = x;

    for (int i = 0; i < font_height; i++) {
        for (int j = 0; j < font_width; j++) {
            if ((font[c * font_height + i] >> (7 - j)) & 1)
                plot_px(x++, y, hex_fg);
            else
                plot_px(x++, y, hex_bg);
        }
        y++;
        x = orig_x;
    }

    return;
}

static void plot_char_grid(int tty, char c, int x, int y, uint32_t hex_fg, uint32_t hex_bg) {
    if (tty == current_tty)
        plot_char(c, x * font_width, y * font_height, hex_fg, hex_bg);
    ttys[tty].grid[x + y * cols] = c;
    ttys[tty].gridfg[x + y * cols] = hex_fg;
    ttys[tty].gridbg[x + y * cols] = hex_bg;
    return;
}

static void clear_cursor(int tty) {
    if (tty == current_tty) {
        plot_char(ttys[tty].grid[ttys[tty].cursor_x + ttys[tty].cursor_y * cols],
            ttys[tty].cursor_x * font_width, ttys[tty].cursor_y * font_height,
            ttys[tty].text_fg_col, ttys[tty].text_bg_col);
    }
    return;
}

static void draw_cursor(int tty) {
    if (tty == current_tty) {
        if (ttys[tty].cursor_status)
            plot_char(ttys[tty].grid[ttys[tty].cursor_x + ttys[tty].cursor_y * cols],
                ttys[tty].cursor_x * font_width, ttys[tty].cursor_y * font_height,
                ttys[tty].cursor_fg_col, ttys[tty].cursor_bg_col);
    }
    return;
}

static void refresh(int tty) {
    if (tty == current_tty) {
        /* interpret the grid and print the chars */
        for (size_t i = 0; i < (size_t)(rows * cols); i++) {
            plot_char_grid(tty, ttys[tty].grid[i], i % cols, i / cols, ttys[tty].gridfg[i], ttys[tty].gridbg[i]);
        }
        draw_cursor(tty);
    }

    return;
}

static void scroll(int tty) {
    /* notify grid */
    for (size_t i = cols; i < (size_t)(rows * cols); i++) {
        ttys[tty].grid[i - cols] = ttys[tty].grid[i];
        ttys[tty].gridbg[i - cols] = ttys[tty].gridbg[i];
        ttys[tty].gridfg[i - cols] = ttys[tty].gridfg[i];
    }
    /* clear the last line of the screen */
    for (size_t i = rows * cols - cols; i < (size_t)(rows * cols); i++) {
        ttys[tty].grid[i] = ' ';
        ttys[tty].gridbg[i] = ttys[tty].text_bg_col;
        ttys[tty].gridfg[i] = ttys[tty].text_fg_col;
    }

    refresh(tty);
    return;
}

static void clear(int tty) {
    for (size_t i = 0; i < (size_t)(rows * cols); i++) {
        ttys[tty].grid[i] = ' ';
        ttys[tty].gridbg[i] = ttys[tty].text_bg_col;
        ttys[tty].gridfg[i] = ttys[tty].text_fg_col;
    }

    refresh(tty);
    return;
}

static void enable_cursor(int tty) {
    ttys[tty].cursor_status = 1;
    draw_cursor(tty);
    return;
}

static void disable_cursor(int tty) {
    ttys[tty].cursor_status = 0;
    clear_cursor(tty);
    return;
}

static void set_cursor_pos(int tty, int x, int y) {
    clear_cursor(tty);
    ttys[tty].cursor_x = x;
    ttys[tty].cursor_y = y;
    draw_cursor(tty);
    return;
}

static void sgr(int tty) {

    if (ttys[tty].esc_value0 >= 30 && ttys[tty].esc_value0 <= 37) {
        ttys[tty].text_fg_col = ansi_colours[ttys[tty].esc_value0 - 30];
        return;
    }

    if (ttys[tty].esc_value0 >= 40 && ttys[tty].esc_value0 <= 47) {
        ttys[tty].text_bg_col = ansi_colours[ttys[tty].esc_value0 - 40];
        return;
    }

    return;
}

static void escape_parse(int tty, char c) {

    if (c >= '0' && c <= '9') {
        *ttys[tty].esc_value *= 10;
        *ttys[tty].esc_value += c - '0';
        *ttys[tty].esc_default = 0;
        return;
    }

    switch (c) {
        case '[':
            return;
        case ';':
            ttys[tty].esc_value = &ttys[tty].esc_value1;
            ttys[tty].esc_default = &ttys[tty].esc_default1;
            return;
        case 'A':
            if (ttys[tty].esc_default0)
                ttys[tty].esc_value0 = 1;
            if (ttys[tty].esc_value0 > ttys[tty].cursor_y)
                ttys[tty].esc_value0 = ttys[tty].cursor_y;
            set_cursor_pos(tty, ttys[tty].cursor_x, ttys[tty].cursor_y - ttys[tty].esc_value0);
            break;
        case 'B':
            if (ttys[tty].esc_default0)
                ttys[tty].esc_value0 = 1;
            if ((ttys[tty].cursor_y + ttys[tty].esc_value0) > (rows - 1))
                ttys[tty].esc_value0 = (rows - 1) - ttys[tty].cursor_y;
            set_cursor_pos(tty, ttys[tty].cursor_x, ttys[tty].cursor_y + ttys[tty].esc_value0);
            break;
        case 'C':
            if (ttys[tty].esc_default0)
                ttys[tty].esc_value0 = 1;
            if ((ttys[tty].cursor_x + ttys[tty].esc_value0) > (cols - 1))
                ttys[tty].esc_value0 = (cols - 1) - ttys[tty].cursor_x;
            set_cursor_pos(tty, ttys[tty].cursor_x + ttys[tty].esc_value0, ttys[tty].cursor_y);
            break;
        case 'D':
            if (ttys[tty].esc_default0)
                ttys[tty].esc_value0 = 1;
            if (ttys[tty].esc_value0 > ttys[tty].cursor_x)
                ttys[tty].esc_value0 = ttys[tty].cursor_x;
            set_cursor_pos(tty, ttys[tty].cursor_x - ttys[tty].esc_value0, ttys[tty].cursor_y);
            break;
        case 'H':
            ttys[tty].esc_value0 -= 1;
            ttys[tty].esc_value1 -= 1;
            if (ttys[tty].esc_default0)
                ttys[tty].esc_value0 = 0;
            if (ttys[tty].esc_default1)
                ttys[tty].esc_value1 = 0;
            if (ttys[tty].esc_value1 >= cols)
                ttys[tty].esc_value1 = cols - 1;
            if (ttys[tty].esc_value0 >= rows)
                ttys[tty].esc_value0 = rows - 1;
            set_cursor_pos(tty, ttys[tty].esc_value1, ttys[tty].esc_value0);
            break;
        case 'm':
            sgr(tty);
            break;
        case 'J':
            switch (ttys[tty].esc_value0) {
                case 2:
                    clear(tty);
                    break;
                default:
                    break;
            }
            break;
        default:
            ttys[tty].escape = 0;
            put_char(tty, '?');
            break;
    }

    ttys[tty].esc_value = &ttys[tty].esc_value0;
    ttys[tty].esc_value0 = 0;
    ttys[tty].esc_value1 = 0;
    ttys[tty].esc_default = &ttys[tty].esc_default0;
    ttys[tty].esc_default0 = 1;
    ttys[tty].esc_default1 = 1;
    ttys[tty].escape = 0;

    return;
}

int tty_write(int tty, const void *void_buf, uint64_t unused, size_t count) {
    (void)unused;
    if (!tty_ready)
        return 0;
    const char *buf = void_buf;
    spinlock_acquire(&ttys[tty].lock);
    for (size_t i = 0; i < count; i++)
        put_char(tty, buf[i]);
    spinlock_release(&ttys[tty].lock);
    return (int)count;
}

static void put_char(int tty, char c) {
    if (ttys[tty].escape) {
        escape_parse(tty, c);
        return;
    }
    switch (c) {
        case '\0':
            break;
        case '\e':
            ttys[tty].escape = 1;
            break;
        case '\t':
            if ((ttys[tty].cursor_x / ttys[tty].tabsize + 1) * ttys[tty].tabsize >= cols)
                break;
            set_cursor_pos(tty, (ttys[tty].cursor_x / ttys[tty].tabsize + 1) * ttys[tty].tabsize, ttys[tty].cursor_y);
            break;
        case '\r':
            set_cursor_pos(tty, 0, ttys[tty].cursor_y);
            break;
        case '\a':
            // dummy handler for bell
            break;
        case '\n':
            if (ttys[tty].cursor_y == (rows - 1)) {
                set_cursor_pos(tty, 0, (rows - 1));
                scroll(tty);
            } else {
                set_cursor_pos(tty, 0, (ttys[tty].cursor_y + 1));
            }
            break;
        case '\b':
            if (ttys[tty].cursor_x || ttys[tty].cursor_y) {
                clear_cursor(tty);
                if (ttys[tty].cursor_x) {
                    ttys[tty].cursor_x--;
                } else {
                    ttys[tty].cursor_y--;
                    ttys[tty].cursor_x = cols - 1;
                }
                draw_cursor(tty);
            }
            break;
        default:
            plot_char_grid(tty, c, ttys[tty].cursor_x++, ttys[tty].cursor_y, ttys[tty].text_fg_col, ttys[tty].text_bg_col);
            if (ttys[tty].cursor_x == cols) {
                ttys[tty].cursor_x = 0;
                ttys[tty].cursor_y++;
            }
            if (ttys[tty].cursor_y == rows) {
                ttys[tty].cursor_y--;
                scroll(tty);
            }
            draw_cursor(tty);
    }

    return;
}