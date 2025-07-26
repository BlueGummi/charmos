# charmos - Compact, Hobbyist And Recreational Monolithic Operating System

<p align="center">
<img src="https://github.com/BlueGummi/charmos/blob/main/charmos.png" width="240">
</p>

## Build

Prerequisites: `nasm` and `xorriso`

```bash
./build.sh

```
## Run

```bash
make run # run this in the build directory
```

## Roadmap 

> The :trophy: means the task is a challenge, the :broom: means something is trivial

### TODO:

- [x] blkdev bcache

- [x] bcache async io prefetch/readahead

- [x] async blkdev io request scheduler

- [x] mlfq smp work stealing scheduler

- [ ] timer api abstraction

- [ ] clean up all drivers. less bitfields, more #defines

- [x] async blkdev io operations

- [ ] lots of acpi things to do stuff

- [ ] devtmpfs 

- [ ] proper system of using ifdefs to log info, track info, do profiling, etc.

- [ ] ports for programs

- [ ] proper userland

- [ ] ia32 port + gub support

- [ ] finish up vfs and add page cache

- [ ] more drivers + finish xhci

- [ ] networking :boom: + networking stack :boom: :boom:

