# Style Guide

This is a style **guide**, not a set of style **rules**. It is not entirely prohibited to have small segments of code that deviate from the guidelines specified here.

## General naming conventions

`snake_case` should be used everywhere, unless an external library with a different naming scheme is used.

Why: Acronyms and prefixes benefit from `snake_case` in readability, and switching between cases isn't beneficial.

For acronyms, it is preferred to maintain a lowercase naming scheme for all letters, unless an external library with a different naming scheme is used.

## Overall consistencies

These are general uniformities and code organization standards that the codebase should seek to mostly uphold.

For things not specified here, such as multi-line comments, simply use the same style as the rest of the file is using.

> A "behavior" is a set of functions that all collectively accomplish a singular, specific task

> A "group" refers to a collection of behaviors under a related context

### Create comments to rationalize and reason, not to explain

Comments should exist to explain **why** something exists or needs to exist. It can be helpful to leave a comment about how something functions sometimes, but do not be overly enthusiastic about leaving comments on every last little thing, as this causes creates unnecessary noise.

Comments in places such as enum members and struct members are encouraged, as many IDEs will generate documentation on the fly from these comments.

If a function has multiple arguments, it is acceptable to call it like this

```c
function(
     /* arg1 = */ arg1,
     /* arg2 = */ arg2,
     /* arg3 = */ arg3);
```
    
### Group by context, sort by behavior

Code should remain in the same directory/group as other code that is operating in related contexts. 
Even if there are different implementations in a group (e.g. FAT and ext2), they should all reside under a parent directory, in this case `kernel/fs`.

Independent files should be based on the behavior of functions in the file. 
e.g., under `kernel/fs/ext2`, the code for reading/writing blocks and inodes are in the same `ext2_io.c` file, whereas the file creation, file deletion, and lookup functions all have their own files.


```bash
include/
└── ext2.h             # header           - group
kernel/
└── fs/                # higher dir       - group
    ├── ext2/          # dir              - group
    │   ├── io.c       # block/inode i/o  - behavior
    │   ├── lookup.c   # file lookup      - behavior
    │   ├── create.c   # file creation    - behavior
```

Headers should correspond to groups, not behaviors. 

### Name headers by group, name sources with optional prefixes

Header files should all contain the overall group of functionality of the header in their name. e.g., it is preferred to call a header "vmm.h", rather than "map_page.h" as the former is a group whereas the latter is a specific behavior.

Source files can optionally have their prefix, primarily depending on if their path already provides sufficient context around the group of the function.
e.g. it isn't necessary to have a prefix on `kernel/fs/ext2/lookup.c`, but it is also not disallowed to keep the prefix.

Headers should use the `#pragma once` guard, as this is widely supported and easier on everyone.

### Prefix functions by group

Kernel-specific implementations of functions typically present in userspace C-stdlibs should have the `k_` or `k` prefix.
This may avoid collisions once userspace functions are introduced, and also clarifies that it is a kernel implementation.

For symbols used in macros, prefer the `__` prefix to avoid clashes, and for symbols provided externally, such as from the linker, prefer the `__` prefix as well. 

Larger, more specific names are acceptable in cases where they provide information and reduce collisions, such as with `ps2_kb_` and `usb_kb_`, as opposed to just `kb_`.

Similar-behaving but independently-operating public implementations of common functions, such as file reading/writing should each have their own corresponding group prefix if necessary to avoid collisions with other functions.
e.g., `fat32_read`, and `ext2_read`.

Static/file-scoped functions should prefer short, concise non-prefixed names to minimize verbosity as they will not be accessible outside of their file.

## Code guidelines

These are specific guidelines that functions and blocks of code should try to abide by.

The maximum column width is 80 columns, as this helps with both readability and in encouraging shorter, more concise, functions.

For a similar reason, indentations are 4 spaces, as they both encourage less complex code blocks, and appear the same across editors as spaces are standardized in width.

For further small details, such as brace conventions, refer to the `.clang-format` file.

### Keep it short, sweet, and straightforward

Ideally, no single file should exceed 2000 lines of code. If there are more than 2000loc in a file, it is time to consider refactoring or regrouping.

Functions with more convoluted control flow should be limited to less lines of code, whereas functions with very straightforward control flow can span more lines of code, such as a function to search the blocks of a node in a filesystem.

### Keep the `struct/enum`, but allow anonymous aliases

Unless the library being used prefers anonymous `typedef`s for structs an,d enums, use explicit struct/enum definitions. This is because `typedef`s will be used for aliases, such as `pte_t`, and it minimizes the confusion of a variable's type

### `goto` is good, but don't overuse it

For functions with a common exit point upon an error or similar behavior, using a `goto` to reach the statements they will all execute instead of copy-pasting code can increase readability and reduce verbosity.

### Macros are best in moderation

Macros should be used for to define constants and in declaring repeated functions such as in the case of interrupt handlers. But, do not use macros to change the syntax of the language.

### Inline functions

Inline functions should be declared with `static inline`, and ideally be placed in header files to avoid multiple definition errors.

As a rule of thumb, an inline function should not have complex control flow and should never exceed 10 lines of code, such as `inb` and `outb`. Larger inline functions may sacrifice space, potentially being more detrimental than the possible speedup they may provide.

### Inline assembly

For large (> 15 line) segments of assembly, it is preferred to create a separate `.S` or `.asm` file. Both ATT and Intel syntaxes are acceptable. It is preferred to use ATT assembly for inline assembly (or for when functionality only provided by ATT assembly is necessary), and Intel assembly for separate files, as it is often viewed as more readable and is typically seen as easier to write.

### Errors should not be too "extra"

In functions that can produce an error in a variety of different ways, but do not benefit from providing information on how they produced an error, simply return a `bool` as to whether the function succeeded or failed.

However, in functions that require detailed error information, such as userspace-exposed APIs, use the provided file in `include/errno.h` to return an error that indicates what the problem was.

In general, use `bool` for internal functions where the caller doesn’t need details, and use error codes for public APIs or syscalls where debuggability matters.

## Tooling

There is a `CMakeLists.txt` and `build.sh`, which contain all relevant information for tooling. Always try to resolve warnings on things besides unused variables, and if it is not possible, leave a comment about the warning. **Don't silence warnings**.

The build script should be complete and simple. It is wasteful to spend time on tooling troubles, and complexity is cumbersome.

## Testing

It is highly preferred to write tests in `#ifdef TESTS` blocks that will only compile if the tests are enabled at compilation. Specific notes on tests, (e.g. flags to pass to QEMU, external disks that should be formatted in a specific way) should be commented around the test portions.

Tests should seek to check edge cases more than general scenarios, as these will more often than not be the source of bugs and errors.

Refer to `include/tests.h` to see how tests are done, and check `kernel/tests` to see examples of tests

## Contributing

Commit messages should provide a brief overview of the change made. Avoid changing many behaviors at once in one commit, and if the commit is large, provide a larger description on its changes.

For specific commit message styles, it isn't strictly enforced to use one or the other, as long as the message itself sufficiently provides information regarding the commit.

The commonly used style of `[scope]: {subject}` is helpful, but it is not prohibited to write commit messages in other styles.

## Miscellaneous

### Integer sizes

In cases where the size of an integer matters, it is better to use the `stdint.h` type, as it is easier to read and understand. However, if a short-lived integer is being used, such as in a loop counter, it is acceptable to use the standard `int` type or other default C types.

### Librarie

If libraries are to be added, it is preferred to add them as submodules. If this is not possible, modify the build script to include them.
