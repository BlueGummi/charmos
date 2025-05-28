# charmos - Compact, Hobbyist And Recreational Microkernel Operating System

<p align="center">
<img src="https://github.com/BlueGummi/charmos/blob/main/charmos.png" width="240">
</p>

## build

`nasm` and `xorriso` need to be present on the machine, currently Linux and macOS only.

```bash
./build.sh

```
## run

```bash
make run # run this in the build directory
```

## roadmap 

> The :trophy: means the task is a challenge, the :broom: means something is trivial

### "The Basics" 

- [x] gdt + idt 

- [x] physical memory allocator 

- [x] virtual memory manager 

    - [x] virtual memory allocator 

- [x] do paging properly 

### "IO Intricacies" 

- [x] uACPI setup 

    - [ ] :trophy: parse acpi tables 

- [x] handle other interrupts + add some 

### "QOL Quarrels" 

- [ ] implement returned error codes for specific error kinds on things like the fs

- [x] change tooling 

    - [x] and reorganize codebase 

    - [x] :broom: and do janitor work to cleanup headers and such 
    
    - [ ] :broom: clean up formatting in the code for readability (newlines and such)

    - [ ] :trophy: formalize a style specification 

- [x] work on some c stdlib-style functions 

    - [ ] :trophy: add more c stdlib functions 

### "Parallel Predicaments" 

- [x] wake up other CPUs 

    - [x] do something useful with them 

        - [x] thread structure

- [x] create a separate thread/task struct

    - [ ] each task has its own address space

    - [x] threads with their own registers and state

    - [x] schedule threads not tasks

- [x] start making tasks 

    - [x] :broom: add more detail to tasks 

- [x] spin lock thing 

    - [x] :broom: lock more things 

    - [ ] schedule kernel events

- [ ] :trophy: multithread more core functions 

### "Task Troubles" 

- [x] beginnings of scheduler + timer 

    - [x] implement task removal
    
    - [ ] :trophy: implement smarter scheduler 

- [x] vfs beginnings

    - [ ] create system calls for interaction with these

### "Persistency Problems" 

- [x] begin filesystem abstractions

    - [x] begin ext2 filesystem

    - [ ] :trophy: complete ext2 filesystem

- [ ] create fat filesystem cuz why not

- [ ] drivers

- [ ] probably should make a shell 

### "Lord of the Third Ring" 

- [ ] enter userspace 

#### to be continued...
