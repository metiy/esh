esh - embedded shell
====================

esh is a lightweight command shell for embedded applications, small enough to
be used for (and intended for) debug UART consoles on microcontrollers.

This readme describes esh and how to try it out. If you think you'd like to
use it, see the [compiling guide](COMPILING.md) for information on how to set
it up and integrate it into your project.

Demo
====

There is a simple demo in the `demo` subdirectory, which can be compiled and
run on a unix-like system by moving into that directory and issuing the `make`
command. There are no dependencies other than libc and a compiler. Once built,
execute `./demo` and try it out!

Features
========

Line editing
------------

esh supports basic line editing, understanding the backspace key to delete
characters, left and right arrow to move the insertion point, and Ctrl-C
to ditch the entire line. Ctrl-left/right to move by a word is upcoming.

Argument tokenizing
-------------------

esh automatically splits a command string into arguments, and understands
bash-style quoting. The command handler callback receives a simple
argc/argv array of arguments, ready to use. Environment variables are not yet
supported, but may be in the future.

History (optional)
------------------

If compiled in, esh supports history, allowing the use of the up/down arrow keys
to browse previously entered commands and edit/re-issue them. A ring buffer is
used to store a fixed number of characters, so more commands can be remembered
if they're shorter. See the [compiling guide](COMPILING.md) for information on
how to set it up.
