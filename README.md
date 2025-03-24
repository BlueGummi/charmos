# kernel - I have not given this kernel a name

## install

make sure u have nasm and xorriso :thumbsup: :zany_face:

clone the repository and then do this

```bash
git submodule init && git submodule update

```

then pull one of these to compile the thingy

```bash
./kernel/get-deps && mkdir -p build && cd build && cmake .. && make iso 
```

now u can run 

```bash
make run
```

in the build directory and it'll bring up qemu with the kernel in it :thumbsup: :DDD B)))



### todo

- [x] gdt + idt

- [x] allocator

- [x] virtual memory manager

- [ ] do paging properly

- [ ] uACPI setup
    - [ ] parse acpi tables

- [ ] handle other interrupts + add some

- [x] change tooling 
    - [ ] and reorganize codebase

- [x] work on some c stdlib-style functions
    - [ ] add more c stdlib functions

- [ ] scheduler + timer

- [ ] vfs

- [ ] probably should make a shell
