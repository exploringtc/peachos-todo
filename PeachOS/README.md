# todo on PeachOS

Little todo-list program I built so it runs on top of PeachOS as a normal
user program (todo.elf), launched from the shell.

It's nothing fancy: add stuff, list it, remove it, save/load to disk.
The point of the project was less about the todo app itself and more
about wiring up a user program end-to-end - syscalls, the int 0x80 path,
disk read/write, and the FAT image build.

## commands inside the app

```
add <task>      add a new task
list            show what's there
remove <id>     drop a task by its id
help            print commands
exit            back to the shell
```

ids are just whatever the kernel handed out, they don't reset when you
remove things - that's intentional so you can't accidentally reuse one
mid-session.

There are also save/load syscalls wired up on the kernel side (with a
little XOR-obfuscated on-disk format), but we kept them out of the
interactive REPL on purpose - the in-class demo stays focused on the
add/list/remove syscall path. See CHANGES_FROM_NIBBLEBITS.txt for the
full story.

## building

```
./build.sh
```

The build script calls into the Makefile, which compiles the kernel,
the stdlib, the shell, and todo, then assembles everything into
`bin/os.bin`. The todo.elf gets `mcopy`'d into the FAT region of the
image so the shell can find it at runtime.

If the build complains about `mtools` or `nasm`, install those first.
On Debian/Ubuntu:

```
sudo apt install nasm mtools qemu-system-x86 build-essential
```

## running

```
qemu-system-i386 -hda PeachOS/bin/os.bin
```

Once it boots into the shell prompt, just type:

```
todo
```

and the app takes over. `exit` drops you back to the shell.

## what was changed vs upstream PeachOS

See `CHANGES_FROM_NIBBLEBITS.txt` for the file-by-file notes. Short
version: added syscalls 10-14, a kernel-side todo backend in
`src/isr80h/src_todo.c`, write support in the disk driver so save/load
actually persists, and a new user program under `programs/todo/`.

## credit

Base kernel is from nibblebits/PeachOS. Everything todo-related (the
user program, the syscall handlers, the disk write path needed for
save/load, and the build glue) is what I added on top for this project.
