# Makefile for the todo user program.
# pattern follows programs/shell/Makefile on purpose so anything you
# learn from one applies to the other. produces todo.elf, which gets
# mcopy'd into the FAT region of os.bin by the top-level Makefile.

FILES = ./build/main.o ./build/todo.o

# stdlib lives one directory up - that's where peachos.h and the
# int 0x80 wrappers come from.
INCLUDES = -I../stdlib/src

# freestanding flags - we don't have a hosted libc, so disable builtins,
# default libs, and startfiles. -O0 keeps the disassembly readable when
# debugging in qemu+gdb. -Werror because silent warnings have bitten us
# more than once.
FLAGS = -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops \
        -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function \
        -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter \
        -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc

# link our objects against the prebuilt stdlib.elf. order matters:
# our objects first, stdlib last so it can satisfy any references.
all: ${FILES}
	i686-elf-gcc -g -T ./linker.ld -o ./todo.elf -ffreestanding -O0 -nostdlib -fpic -g ${FILES} ../stdlib/stdlib.elf

./build/main.o: ./main.c
	mkdir -p ./build
	i686-elf-gcc ${INCLUDES} -I./ $(FLAGS) -std=gnu99 -c ./main.c -o ./build/main.o

./build/todo.o: ./todo.c
	mkdir -p ./build
	i686-elf-gcc ${INCLUDES} -I./ $(FLAGS) -std=gnu99 -c ./todo.c -o ./build/todo.o

clean:
	rm -rf ${FILES}
	rm -f ./todo.elf

