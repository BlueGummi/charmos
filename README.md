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

- [x] async blkdev io operations

- [ ] devtmpfs 

- [ ] port neural network to kernel and use in scheduling

- [ ] remove existing scheduling logic and use neural network scheduler 

- [ ] ports for programs

- [ ] proper userland

- [ ] ia32 port + gub support

- [ ] finish up vfs and add page cache

- [ ] more drivers + finish xhci

- [ ] networking :boom: + networking stack :boom: :boom:

