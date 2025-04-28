# kernel - i have not given this kernel a name

## install

make sure u have nasm and xorriso :thumbsup: :zany_face:

clone the repository and then do this

```bash
git submodule init && git submodule update && git clone https://github.com/limine-bootloader/limine --branch=v9.x-binary --depth=1

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



### todo-list: THE GAME :sunglasses:

#### level 1: "The Basics"

- [x] gdt + idt

- [x] allocator

- [x] virtual memory manager

    - [x] virtual memory allocator

        - [ ] virtual memory slab allocator in rust

- [x] do paging properly

#### level 2: "IO Intricacies"

- [ ] uACPI setup

    - [ ] parse acpi tables

- [x] handle other interrupts + add some

#### dlc: "Toolfoolery and QOL"

- [x] change tooling 

    - [x] and reorganize codebase

    - [ ] and do janitor work to cleanup headers and such

    - [ ] formalize a style specification

- [x] work on some c stdlib-style functions

    - [ ] add more c stdlib functions

#### level 3: "Parallel Predicaments"

- [x] wake up other CPUs

- [x] start making tasks

- [x] spin lock thing

- [ ] multithread more core functions

#### level 4: "Task Troubles"

- [x] beginnings of scheduler + timer

    - [ ] add more robust scheduling that keeps track of more context and doesn't just do a circular LL
    
        - [ ] implement smarter scheduler (maybe)
    
    - [ ] allow an adjustable scheduler timer firing interval

#### level 5: "Persistency Problems"

- [ ] vfs

- [ ] fat filesystem

- [ ] ext2

- [ ] probably should make a shell

#### boss 1: "Lord of the Third Ring"

- [ ] enter userspace
