# Huge Idea: Locking Philosophy

## Credits:

Written 11/20/2025, updated 1/24/2026 

## Audience:

Everyone

> Locking is integral to the design of this operating system, and is arguably one of,
> if not the most important subsystems, thus, it is intended for everyone to read.

## Overview:

Locking is the component of multitasking operating systems and programs that faciliates protection of shared resources.
This OS allows preemption and is SMP (multicore) compatible, which introduces a few more locking problems.

## Background:

### History:

Throughout the history of operating systems, the concept of "virtualizing" physical resources has been front and
center in the design of various OS components. For example, memory management units virtualize memory to
map virtual addresses to physical addresses.

Similarly, the OS "virtualizes" the processor to split it between different tasks.

This is accomplished through "threads", which are effectively "virtual CPUs" that can be created, started, and stopped
at almost any time. The ability for threads to be started and stopped is used to give the illusion of
running multiple threads at the same time on one CPU by rapidly starting, stopping, and switching between them.
This is known as "context switching", which is one component of the much larger concept of "scheduling",
which will be discussed in more detail elsewhere.

### "The Problem":

However, "with great power comes great responsiblity"[^1], and multitasking operating systems and multithreaded programs
come with much responsiblity.

One major struggle that comes with multitasking is synchronization, or the ability to coordinate threads.

One subset of synchronization predicaments is known as "locking". In multitasking SMP
systems, there are often structures which cannot be safely accessed by multiple entities simulatenously.

Thus, the access to such structures and objects must be protected.

Take the following piece of pseudocode as an example:

```c
/* Thread 1: */
struct list_node *first = NULL;
if (list != NULL)
    first = list->head;

/* Thread 2: */
list = NULL;

```

In this example thread 1 is attempting to read the first element of the list and thread 2 is setting the list to NULL.

Although it would seem like this would always be OK because of the NULL pointer check in thread 1, there exists a small
window in between the time that the list is checked to the time that the list is dereferenced where thread 2 can run
and set the list to NULL, thus causing thread 1 to read the invalid pointer and crash :boom:.

Even if the threads are spawned in an order that would make it seem as if one would execute before the other, there is
no guarantee that would be the case by the time they execute that snippet of code.

```c
/* does not guarantee that t1 always runs before t2 */
thread_spawn(t1_entry);
thread_spawn(t2_entry);
```

These scenarios in which timing-dependent events can impact the overall behavior of a program are known as "race conditions".

### "The Solution":

One way to resolve this particular kind of race condition is with a lock.

For example, if we were to rewrite the previous snippet of code (assuming full atomicity of every operation and no memory access
reordering – we'll talk about what those words mean later) with a simple lock, we might write something like this:

> For simplicity's sake, we will define a function `read_lock_and_set_if_not_held` that checks the lock variable, and
> if it is `false`, sets it to `true`, returning `true` if it is successful in this operation, or `false` if not.

> We will also say that the `read_lock_and_set_if_not_held` function happens all at once,
> or atomically, meaning that in the function there exists no window of time in between the
> lock value being read, checked, and set where the lock can change state.

```c
/* Global scope */
bool lock = false;

/* Thread 1: */
while (read_lock_and_set_if_not_held(lock) == false) { /* retry */ }

struct list_node *first = NULL;
if (list)
    first = list->head;

lock = false;



/* Thread 2: */
while (read_lock_and_set_if_not_held(lock) == false) { /* retry */ }

list = NULL;

lock = false;
```

Now there is a variable that the threads are waiting on before they read or modify the list, which guarantees that only one
thread is reading or modifying the list at once.

### Lock Types

There are two primary types of exclusive locks: spin locks and blocking locks.

Blocking locks are typically referred to as "mutexes" (mutual exclusion), and spin locks are referred to as "spin locks".

To avoid confusion, we will refer to them as such from here on out.

The difference between them resides in how contending threads wait on a lock that is held.

In short, threads attempting to acquire a spin lock will spin in a loop, whereas threads attempting
to acquire a mutex will stop running, or yield to let other threads run, while they wait for the lock to be released.

Spin locks and mutexes also have different use cases. In general, mutexes are more
restrictive in when they can be called compared to spin locks.

For example, it is not possible to acquire a mutex from a non-thread context, such as within
an interrupt service routine. However, spin locks can be acquired just fine from thread and non-thread contexts.

In addition to the two types of locks, there are also rules that locks follow. Operating systems often have "priorites"
for threads, and "preemption", which allows higher priority threads to run before lower priority ones.

Spin locks temporarily disable preemption and re-enable it when they are released. This is to prevent a scenario in which
a higher priority thread can indefinitely starve a lower priority thread because it is waiting on a spin lock that
the lower priority thread is holding. This is also referred to as "Priority Inheritance" (or moreso, is a simpler
form of PI), which will be discussed elsewhere.

## Summary

### Lock Rules

Locks have strict rules. For example, is prohibited for a non-owner thread to release a lock. This means
that you cannot do funny things like acquire a lock with one thread and then spawn another to release it.

In addition, per-CPU structures must be protected with locks. Take the following code snippet as an example:

```c
/* Global scope */
struct shared_data lock_me[CPU_COUNT];

/* Thread 1 runs... */
val = lock_me[smp_core_id()];
if (var == CONST)
    /* Thread 1 is about to do something with the structure... */

/* Thread 1 gets preempted... */
/* Thread 2 runs on the same processor... */
lock_me[smp_core_id()] = another_value;
```

Similar to the previous example demonstrating why locks are necessary, this example
shows a similar instance where threads preempting each other on CPU-local variables can still
result in race conditions. Here, the structure can be read on one thread (which is preempted),
modified by another, and incorrectly operated upon based on an old, now invalid value.

You can also use IRQLs[^2] to disable preemption during such code segments to protect the structure.

Locks also cannot be recursively acquired. Some may argue that recursive mutexes are a nicety and make life 
easier, and while that may hold true in certain cases, like when you're working on a legacy codebase that 
uses recursive mutexes and management wants the next release shipped by tomorrow, in our case, it is less than ideal.

This is primarily because recursive mutexes can increase debugging complexity, and also becomes difficult to maintain. 

### Memory Usage

Due to the differences between mutexes and spin locks, and how the former requires much more bookkeeping
to handle the thread blocking and waking, there is a distinct memory usage difference in between the two.

Whilst a spin lock can be implemented with just one singular bit of data (though it is actually more like a byte
or a word because spinning on a single bit is expensive), mutexes require much more.

Mutexes are quite common, but are not always contended. Thus, the multiple bytes of memory they take up
often go mostly unused if the mutex metadata is embedded directly in the struct.

For example:

```c
struct mutex {
    struct thread_queue waiters; /* 16 bytes */
    struct thread *owner; /* 8 bytes */
    struct spinlock lock; /* 1 byte - for thread_queue */
};
```

This relatively simple mutex that is still *omitting* parts of more featureful mutex (e.g. PI data structures)
takes up a whole 25 bytes of data. That means that after creating only 170 of these mutexes, an entire 4KB page of
memory will be used up, compared to the 512 mutexes that you could create if they only took up 8 bytes of memory.

To combat this problem, instead of directly embedding mutex data structures into `struct mutex`'s, we use
turnstiles. Turnstiles give us the ability to have pointer sized mutexes, whilst still allowing for blocking,
priority inheritance, and even reader-writer locks (which would have 2 queues, one for readers and one for writers).

#### Turnstiles

A turnstile is effectively a structure that tracks metadata regarding a lock, including waiter threads,
priority information, and other data.



[^1]: https://en.wikipedia.org/wiki/With_great_power_comes_great_responsibility
[^2]: https://en.wikipedia.org/wiki/IRQL_(Windows)
