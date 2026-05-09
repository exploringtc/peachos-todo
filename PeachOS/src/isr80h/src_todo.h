/* kernel-side syscall handler prototypes for the todo app.
   these are the functions the dispatcher in isr80h.c registers under
   command numbers 10..14. user space reaches them via int 0x80 with
   EAX set to the matching command id - see programs/stdlib/src/peachos.asm.
   keeping the prototypes in their own header means isr80h.c and
   src_todo.c can't disagree on the signatures. */

#ifndef ISR80H_SRC_TODO_H
#define ISR80H_SRC_TODO_H

struct interrupt_frame;

void* isr80h_command10_todo_add(struct interrupt_frame* frame);
void* isr80h_command11_todo_list(struct interrupt_frame* frame);
void* isr80h_command12_todo_remove(struct interrupt_frame* frame);
void* isr80h_command13_todo_save(struct interrupt_frame* frame);
void* isr80h_command14_todo_load(struct interrupt_frame* frame);

#endif