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



## todo-list: THE GAME :sunglasses:

> The :trophy: means the task is a challenge and would earn one challenge point
each level has at least one challenge point to earn

> The :broom: means something is trivial

## Total points: 22/128
### Challenge points: 0/9 

### level 1: "The Basics" (8/13 points)

- [x] gdt + idt (1 point)

- [x] physical memory allocator (1 point)

- [x] virtual memory manager (2 points)

    - [x] virtual memory allocator (2 points)

        - [ ] :trophy: virtual memory slab allocator in rust (5 points) 

- [x] do paging properly (2 points)

### level 2: "IO Intricacies" (2/7 points)

- [ ] uACPI setup (2 points)

    - [ ] :trophy: parse acpi tables (3 points)

- [x] handle other interrupts + add some (2 points)

### dlc: "Toolfoolery and QOL" (5/12 points)

- [x] change tooling (2 points)

    - [x] and reorganize codebase (1 point)

    - [ ] :broom: and do janitor work to cleanup headers and such (1 point)

    - [ ] :trophy: formalize a style specification (3 points)

- [x] work on some c stdlib-style functions (2 points)

    - [ ] :trophy: add more c stdlib functions (3 points)

### level 3: "Parallel Predicaments" (5/15 points)

- [x] wake up other CPUs (2 points)

    - [ ] do something useful with them (1 point)

- [x] start making tasks (2 points)

    - [ ] :broom: add more detail to tasks (1 point)

- [x] spin lock thing (1 point)

    - [ ] :broom: lock more things (1 point)

- [ ] :trophy: multithread more core functions (7 points) 

### level 4: "Task Troubles" (2/21 points)

- [x] beginnings of scheduler + timer (2 points)

    - [ ] add more robust scheduling that keeps track of more context and doesn't just do a circular LL (3 points)
    
        - [ ] :trophy: implement smarter scheduler (maybe) (6 points)
    
    - [ ] allow an adjustable scheduler timer firing interval (2 points)

- [ ] vfs :trophy: (8 points)

### level 5: "Persistency Problems" (0/60 points)

- [ ] :trophy: fat filesystem (10 points)
    
    - [ ] userland interface for fs interaction (5 points)

- [ ] :trophy: ext2 (15 points)

- [ ] probably should make a shell (30 points)

### boss 1: "Lord of the Third Ring" (??? points)

- [ ] enter userspace 

#### to be continued...
