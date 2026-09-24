/* See ui/menu.h. */
#include "ui/menu.h"

static void scroll_into_view(menu *m) {
    if (m->rows < 1) m->rows = 1;
    if (m->sel < m->top) m->top = m->sel;
    if (m->sel >= m->top + m->rows) m->top = m->sel - m->rows + 1;
    if (m->top > m->count - m->rows) m->top = m->count - m->rows;
    if (m->top < 0) m->top = 0;
}

void menu_init(menu *m, int count, int rows) {
    m->count = count < 0 ? 0 : count;
    m->rows  = rows;
    m->sel   = 0;
    m->top   = 0;
    scroll_into_view(m);
}

void menu_select(menu *m, int sel) {
    if (sel >= m->count) sel = m->count - 1;
    if (sel < 0) sel = 0;
    m->sel = sel;
    scroll_into_view(m);
}

void menu_move(menu *m, int delta) {
    int to;

    if (m->count <= 0) return;
    to = m->sel + delta;
    if (to < 0) to = (m->sel == 0 && delta == -1) ? m->count - 1 : 0;
    else if (to >= m->count) to = (m->sel == m->count - 1 && delta == 1) ? 0 : m->count - 1;
    menu_select(m, to);
}

void menu_set_count(menu *m, int count) {
    m->count = count < 0 ? 0 : count;
    menu_select(m, m->sel);
}
