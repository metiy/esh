/*
 * esh - embedded shell
 * Copyright (C) 2017 Chris Pavlina
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define STATIC 1
#define MANUAL 2
#define MALLOC 3

#include <esh.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#define ESH_INTERNAL_INCLUDE
#include <esh_argparser.h>
#include <esh_internal.h>

enum esh_flags {
    IN_ESCAPE = 0x01,
    IN_BRACKET_ESCAPE = 0x02,
    IN_NUMERIC_ESCAPE = 0x04,
};

static esh_t * allocate_esh(void);
static void free_last_allocated(esh_t * esh);
static void do_print_callback(esh_t * esh, char c);
static void do_command(esh_t * esh, int argc, char ** argv);
static void do_overflow_callback(esh_t * esh, char const * buffer);
static bool command_is_nop(esh_t * esh);
static void execute_command(esh_t * esh);
static void handle_char(esh_t * esh, char c);
static void handle_esc(esh_t * esh, char esc);
static void handle_ctrl(esh_t * esh, char c);
static void ins_del(esh_t * esh, char c);
static void term_cursor_move(esh_t * esh, int n);
static void cursor_move(esh_t * esh, int n);
static void word_move(esh_t * esh, int dir);

void esh_default_overflow(esh_t * esh, char const * buffer, void * arg);

#ifdef ESH_STATIC_CALLBACKS
extern void ESH_PRINT_CALLBACK(esh_t * esh, char c, void * arg);
extern void ESH_COMMAND_CALLBACK(
    esh_t * esh, int argc, char ** argv, void * arg);
__attribute__((weak))
void ESH_OVERFLOW_CALLBACK(esh_t * esh, char const * buffer, void * arg)
{
    (void) arg;
    esh_default_overflow(esh, buffer, arg);
}
#else
void esh_register_command(esh_t * esh, esh_cb_command callback)
{
    esh->cb_command = callback;
}


void esh_register_print(esh_t * esh, esh_cb_print callback)
{
    esh->print = callback;
}


void esh_register_overflow(esh_t * esh, esh_cb_overflow overflow)
{
    esh->overflow = (overflow ? overflow : &esh_default_overflow);
}
#endif

// API WARNING: This function is separately declared in lib.rs
void esh_set_command_arg(esh_t * esh, void * arg)
{
    esh->cb_command_arg = arg;
}

// API WARNING: This function is separately declared in lib.rs
void esh_set_print_arg(esh_t * esh, void * arg)
{
    esh->cb_print_arg = arg;
}

// API WARNING: This function is separately declared in lib.rs
void esh_set_overflow_arg(esh_t * esh, void * arg)
{
    esh->cb_overflow_arg = arg;
}

static void do_print_callback(esh_t * esh, char c)
{
#ifdef ESH_STATIC_CALLBACKS
    ESH_PRINT_CALLBACK(esh, c, esh->cb_print_arg);
#else
    esh->print(esh, c, esh->cb_print_arg);
#endif
}


static void do_command(esh_t * esh, int argc, char ** argv)
{
#ifdef ESH_STATIC_CALLBACKS
    ESH_COMMAND_CALLBACK(esh, argc, argv, esh->cb_command_arg);
#else
    esh->cb_command(esh, argc, argv, esh->cb_command_arg);
#endif
}


static void do_overflow_callback(esh_t * esh, char const * buffer)
{
#ifdef ESH_STATIC_CALLBACKS
    ESH_OVERFLOW_CALLBACK(esh, buffer, esh->cb_overflow_arg);
#else
    esh->overflow(esh, buffer, esh->cb_overflow_arg);
#endif
}


/**
 * For the static allocator, this is global so free_last_allocated() can
 * decrement it.
 */
#if ESH_ALLOC == STATIC
static size_t n_allocated = 0;
#endif


/**
 * Allocate a new esh_t, or return a new statically allocated one from the pool.
 * This does not perform initialization.
 */
static esh_t * allocate_esh(void)
{
#if ESH_ALLOC == STATIC
    static esh_t instances[ESH_INSTANCES];

    if (n_allocated < ESH_INSTANCES) {
        esh_t * esh = &instances[n_allocated];
        ++n_allocated;
        return esh;
    } else {
        return NULL;
    }
#elif ESH_ALLOC == MALLOC
    return malloc(sizeof(esh_t));
#else
#   error "ESH_ALLOC must be STATIC or MALLOC"
#endif
}


/**
 * Free the last esh_t that was allocated, in case an initialization error
 * occurs after allocation.
 */
static void free_last_allocated(esh_t *esh)
{
#if ESH_ALLOC == STATIC
    (void) esh;
    if (n_allocated) {
        --n_allocated;
    }
#elif ESH_ALLOC == MALLOC
    free(esh);
#endif
}


// API WARNING: This function is separately declared in lib.rs
esh_t * esh_init(void)
{
    esh_t * esh = allocate_esh();

    memset(esh, 0, sizeof(*esh));
#ifndef ESH_STATIC_CALLBACKS
    esh->overflow = &esh_default_overflow;
#endif

    if (esh_hist_init(esh)) {
        free_last_allocated(esh);
        return NULL;
    } else {
        return esh;
    }
}


// API WARNING: This function is separately declared in lib.rs
void esh_rx(esh_t * esh, char c)
{
    if (esh->flags & (IN_BRACKET_ESCAPE | IN_NUMERIC_ESCAPE)) {
        handle_esc(esh, c);
    } else if (esh->flags & IN_ESCAPE) {
        if (c == '[' || c == 'O') {
            esh->flags |= IN_BRACKET_ESCAPE;
        } else {
            esh->flags &= ~(IN_ESCAPE | IN_BRACKET_ESCAPE);
        }
    } else {
        // Verify the c is valid non-extended ASCII (and thus also valid
        // UTF-8, for Rust), regardless of whether this platform's isprint()
        // accepts things above 0x7f.
        if (c >= 0x20 && (unsigned char) c <= 0x7f) {
            handle_char(esh, c);
        } else {
            handle_ctrl(esh, c);
        }
    }
}


/**
 * Process a normal text character. If there is room in the buffer, it is
 * inserted directly. Otherwise, the buffer is set into the overflow state.
 */
static void handle_char(esh_t * esh, char c)
{
    esh_hist_substitute(esh);

    if (esh->cnt < ESH_BUFFER_LEN) {
        ins_del(esh, c);
    } else {
        // If we let esh->cnt keep counting past the buffer limit, it could
        // eventually wrap around. Let it sit right past the end, and make sure
        // there is a NUL terminator in the buffer (we promise the overflow
        // handler that).
        esh->cnt = ESH_BUFFER_LEN + 1;

        // Note that the true buffer length is actually one greater than
        // ESH_BUFFER_LEN (which is the number of characters NOT including the
        // terminator that it can hold).
        esh->buffer[ESH_BUFFER_LEN] = 0;
    }
}


/**
 * Process a single-character control byte.
 */
static void handle_ctrl(esh_t * esh, char c)
{
    switch (c) {
        case 27: // escape
            esh->flags |= IN_ESCAPE;
            break;
        case 3:  // ^C
            esh_puts(esh, FSTR("^C\n"));
            esh_print_prompt(esh);
            esh->cnt = esh->ins = 0;
            break;
        case '\n':
            execute_command(esh);
            break;
        case 8:     // backspace
        case 127:   // delete
            esh_hist_substitute(esh);
            if (esh->cnt > 0 && esh->cnt <= ESH_BUFFER_LEN) {
                ins_del(esh, 0);
            }
            break;
        default:
            // nop
            ;
    }
}


/**
 * Process the last character in an escape sequence.
 */
static void handle_esc(esh_t * esh, char esc)
{
    int cdelta;

    if (esc >= '0' && esc <= '9') {
        esh->flags |= IN_ESCAPE | IN_BRACKET_ESCAPE | IN_NUMERIC_ESCAPE;
        return;
    }

    if (esh->flags & IN_NUMERIC_ESCAPE) {
        // Numeric escapes can contain numbers and semicolons; they terminate
        // at letters and ~
        if (esc == '~' || isalpha(esc)) {
            esh->flags &= ~(IN_BRACKET_ESCAPE | IN_NUMERIC_ESCAPE | IN_ESCAPE);
        }
    } else {
        esh->flags &= ~(IN_BRACKET_ESCAPE | IN_NUMERIC_ESCAPE | IN_ESCAPE);
    }

    switch (esc) {
    case ESCCHAR_UP:
    case ESCCHAR_DOWN:
        if (esc == ESCCHAR_UP) {
            ++esh->hist.idx;
        } else if (esh->hist.idx) {
            --esh->hist.idx;
        }
        if (esh->hist.idx) {
            int offset = esh_hist_nth(esh, esh->hist.idx - 1);
            if (offset >= 0 || esc == ESCCHAR_DOWN) {
                esh_hist_print(esh, offset);
            } else if (esc == ESCCHAR_UP) {
                // Don't overscroll the top
                --esh->hist.idx;
            }
        } else {
            esh_restore(esh);
        }
        break;

    case ESCCHAR_LEFT:
        cdelta = -1;
        goto cmove;
    case ESCCHAR_RIGHT:
        cdelta = 1;
        goto cmove;
    case ESCCHAR_HOME:
        cdelta = -esh->ins;
        goto cmove;
    case ESCCHAR_END:
        cdelta = esh->cnt - esh->ins;
        goto cmove;
    case ESCCHAR_CTRLLEFT:
        cdelta = -1;
        goto wmove;
    case ESCCHAR_CTRLRIGHT:
        cdelta = 1;
        goto wmove;
    }

    return;
cmove: // micro-optimization, yo!
    cursor_move(esh, cdelta);
    return;
wmove:
    word_move(esh, cdelta);
}


/**
 * Return whether the command in the edit buffer is a NOP and should be ignored.
 * This does not substitute the selected history item.
 */
static bool command_is_nop(esh_t * esh)
{
    for (size_t i = 0; esh->buffer[i]; ++i) {
        if (esh->buffer[i] != ' ') {
            return false;
        }
    }
    return true;
}


/**
 * Process the command in the buffer and give it to the command callback. If
 * the buffer has overflowed, call the overflow callback instead.
 */
static void execute_command(esh_t * esh)
{
    // If a command from the history is selected, put it in the edit buffer.
    esh_hist_substitute(esh);

    if (esh->cnt >= ESH_BUFFER_LEN) {
        do_overflow_callback(esh, esh->buffer);
        esh->cnt = esh->ins = 0;
        esh_print_prompt(esh);
        return;
    } else {
        esh->buffer[esh->cnt] = 0;
    }

    esh_putc(esh, '\n');

    if (!command_is_nop(esh)) {
        esh_hist_add(esh, esh->buffer);

        int argc = esh_parse_args(esh);

        if (argc > ESH_ARGC_MAX) {
            do_overflow_callback(esh, esh->buffer);
        } else if (argc > 0) {
            do_command(esh, argc, esh->argv);
        }
    }

    esh->cnt = esh->ins = 0;
    esh_print_prompt(esh);
}


void esh_print_prompt(esh_t * esh)
{
    esh_puts(esh, FSTR(ESH_PROMPT));
}


/**
 * Default overflow callback. This just prints a message.
 */
// API WARNING: This function is separately declared in lib.rs
void esh_default_overflow(esh_t * esh, char const * buffer, void * arg)
{
    (void) buffer;
    (void) arg;
    esh_puts(esh, FSTR("\nesh: command buffer overflow\n"));
}


bool esh_putc(esh_t * esh, char c)
{
    do_print_callback(esh, c);
    return false;
}


bool esh_puts(esh_t * esh, char const AVR_ONLY(__memx) * s)
{
    for (size_t i = 0; s[i]; ++i) {
        esh_putc(esh, s[i]);
    }
    return false;
}


void esh_restore(esh_t * esh)
{
    esh_puts(esh, FSTR(ESC_ERASE_LINE "\r")); // Clear line
    esh_print_prompt(esh);
    esh->buffer[esh->cnt] = 0;
    esh_puts(esh, esh->buffer);
    term_cursor_move(esh, -(int)(esh->cnt - esh->ins));
}


#ifdef ESH_RUST
// API WARNING: This function is separately declared in lib.rs
size_t esh_get_slice_size(void)
{
    return sizeof (struct char_slice);
}
#endif


/**
 * Move only the terminal cursor. This does not move the insertion point.
 */
static void term_cursor_move(esh_t * esh, int n)
{
    for ( ; n > 0; --n) {
        esh_puts(esh, FSTR(ESC_CURSOR_RIGHT));
    }

    for ( ; n < 0; ++n) {
        esh_puts(esh, FSTR(ESC_CURSOR_LEFT));
    }
}


/**
 * Move the esh cursor. This applies history substitution, moves the terminal
 * cursor, and moves the insertion point.
 */
static void cursor_move(esh_t * esh, int n)
{
    esh_hist_substitute(esh);
    if ((int) esh->ins + n < 0) {
        n = -esh->ins;
    } else if ((int) esh->ins + n > (int) esh->cnt) {
        n = esh->cnt - esh->ins;
    }

    term_cursor_move(esh, n);
    esh->ins += n;
}


/**
 * Move the esh cursor backwards or forwards one word. This applies history
 * substitution, moves the terminal cursor, and moves the insertion point.
 *
 * @param dir - move forward if +1 or negative if -1. All other numbers produce
 *              undefined behavior.
 */
static void word_move(esh_t * esh, int dir)
{
    size_t ins = esh->ins;
    esh_hist_substitute(esh);

    if (dir == 0) {
        return;
    } else if (dir < 0) {
        for (; ins > 0 && esh->buffer[ins - 1] == ' '; --ins);
        for (; ins > 0 && esh->buffer[ins - 1] != ' '; --ins);
    } else {
        const size_t cnt = esh->cnt;
        for (; ins < cnt && esh->buffer[ins] != ' '; ++ins);
        for (; ins < cnt && esh->buffer[ins] == ' '; ++ins);
    }

    term_cursor_move(esh, ins - esh->ins);
    esh->ins = ins;
}


/**
 * Either insert or delete a character at the current insertion point.
 * @param esh - esh instance
 * @param c - character to insert, or 0 to delete
 */
static void ins_del(esh_t * esh, char c)
{
    int sgn = c ? 1 : -1;
    bool move = (esh->ins != esh->cnt);

    memmove(&esh->buffer[esh->ins + sgn], &esh->buffer[esh->ins],
            esh->cnt - esh->ins);

    if (c) {
        esh->buffer[esh->ins] = c;
    }

    esh->cnt += sgn;
    esh->ins += sgn;

    if (move) {
        esh_restore(esh);
    } else if (!c) {
        esh_puts(esh, FSTR("\b \b"));
    } else {
        esh_putc(esh, c);
    }
}
