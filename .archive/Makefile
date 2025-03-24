all:
	nasm -f elf32 boot.asm -o boot.o
	gcc -m32 -c kernel.c -o kernel.o -ffreestanding
	ld -m elf_i386 -T linker.ld -o kernel boot.o kernel.o
clean:
	rm -rf *.o
	rm -rf kernel
