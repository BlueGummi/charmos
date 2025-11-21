# Huge Idea: Locking

# Credits:

Written 11/20/2025, updated 11/20/2025

# Audience:

Everyone

> Locking is integral to the design of this operating system, and is arguably one of, 
  if not the most important, thus, it is intended for everyone to read.

# Overview:

Locking is the component of multitasking operating systems and programs that faciliates protection of shared resources.

# Background:

## History:

Throughout the history of operating systems, the concept of "virtualizing" physical resources has been front and
center in the design of the various components. For example, modern memory management units virtualize memory to
map virtual addresses to physical addresses. 

Similarly, operating systems "virtualize" the processor to split it between different tasks.

This is accomplished through "threads", which are effectively "virtual CPUs" that can be created, started, and stopped
at almost any time. The ability for threads to be started and stopped is used to run multiple threads at the same time on one
CPU by rapidly starting, stopping, and switching between threads. This is known as "context switching", which is one
component of the much larger concept of "scheduling", which will be discussed in more detail elsewhere.

## "The Problem":

However, "with great power comes great responsiblity", and multitasking operating systems and multithreaded programs
come with much responsiblity. 

One major struggle that comes with the multitasking is synchronization, or the ability to coordinate threads.

One subset of synchronization predicaments is known as "locking". In multitasking SMP (Shared Memory/Symmetric
Multi-Processing) systems, there are oftentimes structures which cannot be safely accessed by multiple entities simulatenously.

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

Although it seems like this would always succeed because of the NULL pointer check in thread 1, there exists a small
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

## "The Solution":

One way to resolve this particular kind of race condition is with a lock.

At a high level, you can think of locks as variables that "indicate if another variable is under modification". 

For example, if we were to rewrite the previous snippet of code (assuming full atomicity of every operation and no memory 
reordering – we'll talk about what those words mean later) with a simple lock, we might write something like this:

> For simplicity's sake, we will define a function `read_lock_and_set_if_not_held` that checks the lock variable, and 
if it is `false`, sets it to `true`, returning `true` if it is successful in this operation, or `false` if not.

> We will also say that the `read_lock_and_set_if_not_held` function happens all at once, 
or atomically, meaning that in the function there exists no window of time in between the 
lock value being read, checked, and set where the lock can change state.

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

## More Info

There are two primary types of locks, spin locks and blocking locks. 

The difference between them resides in how they wait on a lock. 

In short, threads attempting to acquire a spin lock will spin in a loop, whereas threads attempting
to acquire a blocking lock will stop running, or yield, while they wait for the lock to be released.

Spin locks and blocking locks also have different use cases. In general, blocking locks are more
restrictive in how they can be called compared to spin locks.

For example, it is not possible to acquire a blocking lock from a non-thread context, such as within
an interrupt service routine. However, spin locks can be acquired just fine from thread and non-thread contexts.

In addition to the two types of locks, there are also rules that locks follow. Operating systems often have "priorites"
for threads, and "preemption", which allows higher priority threads to run before lower priority ones. 

Spin locks temporarily disable preemption and re-enable it when they are released. This is to prevent a scenario in which
a higher priority thread can indefinitely starve a lower priority thread because it is waiting on a spin lock that 
the lower priority thread is holding. This is also referred to as "Priority Inheritance", which will be discussed another day.

Locks also have strict acquisition rules. It is prohibited for a non-owner thread to release a lock. This means that you
cannot do funny things like acquire a lock with one thread and then spawn another to release it for you.



# Summary


