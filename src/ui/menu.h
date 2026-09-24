/* A list's selection and scroll position, apart from any drawing. Pure. */
#ifndef UI_MENU_H
#define UI_MENU_H

typedef struct {
    int count; /* entries in the list */
    int rows;  /* how many fit on the screen at once */
    int sel;   /* the selected entry, 0 .. count-1 (0 when empty) */
    int top;   /* the first entry on screen */
} menu;

void menu_init(menu *m, int count, int rows);

/* Moves the selection by `delta`, stopping at either end rather than
 * wrapping -- except that a step past the end from the very last entry goes
 * to the first, and the other way round, so a long list is never more than
 * one press from its other end. Keeps the selection on screen. */
void menu_move(menu *m, int delta);

/* Changes the entry count (a list reloaded) and keeps the selection valid. */
void menu_set_count(menu *m, int count);

/* Puts the selection on `sel`, clamped, and scrolls it into view. */
void menu_select(menu *m, int sel);

#endif /* UI_MENU_H */
