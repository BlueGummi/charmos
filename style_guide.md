# Style Guide

## General naming conventions

`snake_case` should be used everywhere, unless an external library with a different naming scheme is used.

Why: Acronyms and prefixes benefit from `snake_case` in readability, and switching between cases isn't beneficial.

For acronyms, it is preferred to maintain a lowercase naming scheme for all letters, unless an external library with a different naming scheme is used.

## Overall consistencies

These are general uniformities and code organization standards that the codebase should seek to mostly uphold.

For things not specified here, such as multi-line comments, simply use the same style as the rest of the file is using.

> A "behavior" is a set of functions that all collectively accomplish a singular, specific task

> A "group" refers to a collection of behaviors under a related context

### Create comments for code comprehension, not for explanation

Comments should exist to explain **why** something exists, not **how** it functions. Avoid writing comments in the following style, or similar styles:
```c
/* @param: some parameter
* desc: some description
* @return: some return value
*/
```
This greatly sacrifices space, and provides minimal information. The signature of a function should give all this information.

Comments in places such as enum members and struct members are encouraged, as many IDEs will generate documentation on the fly from these comments.
    
### Group by context, sort by behavior

Code should remain in the same directory/group as other code that is operating in related contexts. 
Even if there are different implementations in a group (e.g. FAT and ext2), they should all reside under a parent directory, in this case `kernel/fs`.

Independent files should be based on the behavior of functions in the file. 
e.g., under `kernel/fs/ext2`, the code for reading/writing blocks and inodes are in the same `ext2_rw.c` file, whereas the file creation, file deletion, and lookup functions all have their own files.


```bash
include/
└── ext.h                   # header
kernel/
└── fs/
    ├── ext2/
    │   ├── ext2_rw.c       # block/inode r/w 
    │   ├── ext2_lookup.c   # file lookup 
    │   ├── ext2_create.c   # file creation 
```

Headers should correspond to groups, not behaviors. 

### Name headers by group, name sources with optional prefixes

Header files should all contain the overall group of functionality of the header in their name. e.g., it is preferred to call a header "vmm.h", rather than "map_page.h" as the former is a group whereas the latter is a specific behavior.

Source files can optionally have their prefix, primarily depending on if their path already provides sufficient context around the group of the function.
e.g. it isn't necessary to have a prefix on `kernel/fs/ext/ext2_lookup.c`, but it is also not disallowed to keep the prefix.

### Prefix functions by group, prefer shortened private functions

Kernel-specific implementations of functions typically present in userspace C-stdlibs should have the `k_` or `k` prefix.
This may avoid collisions once userspace functions are introduced, and also clarifies that it is a kernel implementation.

Larger, more specific names are acceptable in cases where they provide information and reduce collisions, such as with `ps2_kb_` and `usb_kb_`, as opposed to just `kb_`.

Similar-behaving but independently-operating public implementations of common functions, such as file reading/writing should each have their own corresponding group prefix if necessary to avoid collisions with other functions.
e.g., `fat32_read`, and `ext2_read`.

Static/file-scoped functions should prefer short, concise non-prefixed names to minimize verbosity as they will not be accessible outside of their file.

## Code guidelines

These are specific rules that functions and blocks of code should try to abide by.

For items left unspecified, such as column width, refer to the `.clang-format` file

### Keep it short, sweet, and straightforward

Ideally, no single file should exceed 600 lines of code. If there is more than 600loc in a file, it is time to consider refactoring or regrouping.

Functions should seek to be under 300 lines of code. Longer functions are only reasonable if they have a sequential "flow" that can be easily followed, such as a long function to search all blocks of a filesystem node. Long functions with a variety of different "flows" within them, such as a function that searches many locations to find something, should be avoided.

### Keep the `struct/enum`, but allow anonymous aliases

Unless the library being used prefers anonymous `typedef`s for structs and enums, use explicit struct/enum definitions. This is because `typedef`s will be used for aliases, such as `pte_t`, and it minimizes the confusion of a variable's type

### `goto` is good, but don't overuse it

For functions with a common exit point upon an error or similar behavior, using a `goto` to reach the statements they will all execute instead of copy-pasting code can increase readability and reduce verbosity.

Avoid going "backwards" with `goto`s, as this can introduce strange and unexpected consequences.

### Macros are best in moderation

Macros should be used for to define constants and in declaring repeated functions such as in the case of interrupt handlers. But, do not use macros to change the syntax of the language or create macros that will only be used one time.

### Inline functions

Inline functions should be declared with `static inline`, and ideally be placed in header files to avoid multiple definition errors.

As a rule of thumb, an inline function should not have complex control flow and should never exceed 10 lines of code, such as `inb` and `outb`. Larger inline functions may sacrifice space, potentially being more detrimental than the possible speedup they may provide.

### Inline assembly

For large (>15) segments of assembly, it is preferred to create a separate `.S` or `.asm` file. Both ATT and Intel syntaxes are acceptable. It is preferred to use ATT assembly for inline assembly (or for when functionality only provided by ATT assembly is necessary), and Intel assembly for separate files, as it is often viewed as more readable and is typically seen as easier to write.

### Assertions

Assertions are helpful in guarding against specific conditions, but, it is preferred to reduce their usage once a function is complete and no longer being actively worked on, as they can reduce readability, and if a proper error handling system exists, can be unnecessary.

### Threads and concurrency

For functions that may be accessed by many threads at once, use a global `struct spin_lock` for that function, and place a lock and unlock around atomic blocks of code. Avoid simply locking the entire function (unless it is necessary), as this can entirely remove the benefit of multithreading.

If interrupts are disabled before setting a lock, do not enable them at the unlock. But, if interrupts are enabled before setting a lock, disable them and re-enable them at the unlock.

### Errors should not be too "extra"

In functions that can produce an error in a variety of different ways, but do not benefit from providing information on how they produced an error, simply return a `bool` as to whether the function succeeded or failed.

However, in functions that require detailed error information, such as userspace-exposed APIs, use the provided file in `include/errno.h` to return an error that indicates what the problem was.

## Tooling

There is a `CMakeLists.txt` and `build.sh`, which contain all relevant information for tooling. Always try to resolve warnings on things besides unused variables, and if it is not possible, leave a comment about the warning. **Don't silence warnings**.

The build script should be complete and simple. It is wasteful to spend time on tooling troubles, and complexity is cumbersome.

## Testing

It is highly preferred to write tests in `ifdef` blocks that will only compile if the tests are enabled at compilation. Specific notes on tests, (e.g. flags to pass to QEMU, external disks that should be formatted in a specific way) should be commented around the test portions

## Miscellaneous

### Citing sources

As this project is under the GPL-3.0, a more restrictive OSS license, it is permissible to take code from similarly-licensed or less restrictively licensed projects. However, include the name, link, and path (and preferably a thank you note) in a comment when code from another source or repository is used. Contributors must ensure their contributions are compatible with the GPL-3.0 license.

### Libraries

If libraries are to be added, it is preferred to add them as submodules. If this is not possible, modify the build script to include them.
