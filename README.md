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

- [ ] fix rcu

- [ ] figure out how to represent processor topology (NUMA, SMT)

- [ ] complete usb devices, xhci, ehci, uhci, ohci

- [ ] clean up all drivers. less bitfields, more #defines

- [x] async blkdev io operations

- [ ] lots of acpi things to do stuff - C states

- [ ] devtmpfs 

- [ ] proper system of using ifdefs to log info, track info, do profiling, etc.

- [ ] ports for programs

- [ ] proper userland

- [ ] ia32 port + gub support

- [ ] finish up vfs and add page cache - dual cache model, metadata in private block cache, file data in page cache

- [ ] networking :boom: + networking stack :boom: :boom:

