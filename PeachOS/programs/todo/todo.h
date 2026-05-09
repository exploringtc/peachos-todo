#ifndef TODO_APP_H
#define TODO_APP_H

/* todo_run - the whole user-facing program lives behind this one call.
   it prints a banner, then loops on the prompt reading one command at
   a time. add/list/remove are forwarded to the kernel through int 0x80
   wrappers; help and exit are handled locally. returns when the user
   types exit (after calling peachos_exit, which doesn't actually come
   back, but the return is there so main has something sane to give back
   to the loader). */
int todo_run(void);

#endif
