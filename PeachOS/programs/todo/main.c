/* entry point for todo.elf - kept deliberately tiny.
   all the actual command-loop work happens in todo_run() over in todo.c.
   the only thing this file is here to do is be the symbol the loader
   jumps to once stdlib's _start has set things up. */

#include "todo.h"
#include "peachos.h"

int main(void)
{
    return todo_run();
}
