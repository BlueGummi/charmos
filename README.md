# charmos - Compact, Hobbyist, And Recreational Microkernel Operating System

A from-scratch OS

## install

make sure u have nasm and xorriso :thumbsup: :zany_face:

clone the repository and then do this

```bash
./build.sh

```

it's that shrimple

now u can run 

```bash
make run
```

in the build directory and it'll bring up qemu with the kernel in it :thumbsup: :DDD B)))



## todo-list: THE GAME :sunglasses:

> The :trophy: means the task is a challenge and would earn one challenge point
each level has at least one challenge point to earn

> The :broom: means something is trivial

### level 1: "The Basics" 

- [x] gdt + idt 

- [x] physical memory allocator 

- [x] virtual memory manager 

    - [x] virtual memory allocator 

- [x] do paging properly 

### level 2: "IO Intricacies" 

- [ ] uACPI setup 

    - [ ] :trophy: parse acpi tables 

- [x] handle other interrupts + add some 

### dlc: "Toolfoolery and QOL" 

- [x] change tooling 

    - [x] and reorganize codebase 

    - [ ] :broom: and do janitor work to cleanup headers and such 
    
    - [ ] :broom: clean up formatting in the code for readability (newlines and such)

    - [ ] :trophy: formalize a style specification 

- [x] work on some c stdlib-style functions 

    - [ ] :trophy: add more c stdlib functions 

### level 3: "Parallel Predicaments" 

- [x] wake up other CPUs 

    - [x] do something useful with them 

        - [ ] thread structure

- [x] start making tasks 

    - [x] :broom: add more detail to tasks 

- [x] spin lock thing 

    - [x] :broom: lock more things 

    - [ ] schedule kernel events

- [ ] :trophy: multithread more core functions 

### level 4: "Task Troubles" 

- [x] beginnings of scheduler + timer 

    - [x] implement task removal
    
    - [ ] :trophy: implement smarter scheduler 

- [x] vfs beginnings

    - [ ] create system calls for interaction with these

### level 5: "Persistency Problems" 

- [ ] :trophy: fat filesystem 

- [ ] drivers

- [ ] probably should make a shell 

### boss 1: "Lord of the Third Ring" 

- [ ] enter userspace 

#### to be continued...
