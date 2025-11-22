# Documentation Guide

Similar to the **style guide**, this is a documentation *guide*, not a set of documentation *rules*. There may be
instances where documentation may deviate from this guide, and that is expected and allowed, but try to follow these
guidelines with most things.

# File titling and other naming

To title a file such that it shows up with a name other than its file name in documentation, simply write

```c
/* @title: Title */
```

anywhere in the file.

To give directories a name other than the directory name itself in documentation, simply create a file called
`dir_doc_name` (no file extension) in the directory, and write the chosen name inside of it.

# What is the format?

Documentation for this codebase should be written in a unified format. This format will be referred to as the "**Idea Structure**".

The description, rationale, and examples of the **Idea Structure** will be laid out and explained below.

## What is the **Idea Structure**?

Code can almost always be sorted into different **components**. These **components** can come together and make **larger
components**, which can continue to create even larger pieces.

Unifying a format for describing and documenting code helps provide *codebase consistency*, makes *documentation
predictable*, and offers a *useful checklist of items* to reduce the chance that the documentation misses something.

The **Idea Structure** defines "**Ideas**" of different "**Sizes**". As the **Size** of an **Idea** increases, the
*scope* that it covers increases, but the *granularity* and *specificity* of the **Idea** decreases.

There are three main **Sizes** of **Ideas**, each having their own use case:

- **Huge Idea**: describes *abstract concepts and philosophies*, does *not* provide implementation details (e.g. memory allocator philosophies)
- **Big Idea**: describes *one significant component of a subsystem* and implementation details (e.g. the slab allocator)
- **Small Idea**: describes *sets of functions or singular functions* of a subsystem (e.g. the garbage collector main loop)

## Where do I put **Ideas**?

ALL **Huge Ideas** should go in the separate `./docs/` directory for documentation. These files should have a `.md`
extension and be written in standard markdown format. The names should be structured as such:

`<Idea Name>_idea.md`

e.g.

`locking_idea.md`

To adhere to the style guide, all file names for **Huge Ideas** in the `./docs/` directory should be lowercase
`snake_case`.

**Big Ideas** and **Small Ideas** should always be in code comments. **Small Ideas** should be "spatially near" the code that they are related to.

### **Big Ideas** and **Small Ideas** MUST go in `./include/` if they are intended to be visible from the documentation website.

> **Ideas** in `./kernel/` will only be visible to those viewing the source code.

In code, **Big Ideas** should come before everything else, even before `include`, `#pragma once`, etc. unless there
is truly a reason why it would not be possible to write the **Idea** without putting it underneath the first few lines.

## What is the format of an **Idea**?

The different pieces of an **Idea** will be referred to as "**Sections**".

**Ideas** follow a common format, but at each **Size**, **Sections** of the **Idea** are added and removed.

In general, each Idea MUST include the following **Sections**.

- Signature: magic signature for an **Idea** so that it can easily be parsed
- Name: what is this **Idea**'s name?
- Overview/problem: one or two sentences that describe in simple language what the **Idea** is about

**Ideas** MAY include these **Sections**, optionally:
- Credits: who wrote this **Idea**?
- Status: status of the **Idea**. if there is no status, the **Idea** is assumed to be stable.
- Alerts: important information about the **Idea** (e.g. why is this unstable?)
- References: other **Ideas** mentioned
- Notes: other things not covered by other **Sections** of the **Idea**
- Audience: who is supposed to read this **Idea**?
- Diagrams: section for diagrams and names of diagrams
- Changelog: history of changes
- Bugs: relevant bug numbers and names
- Tests: relevant tests (list of source code files and short descriptions)

Any **Idea** may define custom subsections underneath larger sections.

## What about text formatting?

In code, **Ideas** **Sections** should be formatted as follows:

```c
/*
 * < space >
 * # Section Name One: Single line body
 * < space >
 * # Section Name Two:
 *   Multi
 *   Line
 *   Body
 * < space >
 *   ## Subsection Header:
 *   Multi
 *   Line
 *   Body
 * < space >
 */
```

Note the two spaces of indentation for the multi-line body and empty lines in between **Sections**.

## When should I write an **Idea**?

**Ideas** should only be written when needed.

Avoid writing a **Small Idea** for a function if it is self-evident what it does (e.g. a print function).

Avoid writing a **Big Idea** when a subsystem has very little internal complexity (e.g. `string.c`, which contains `memset`, `memcpy`, etc.).

## What about the **Idea**'s status?

**Idea** statuses are used to define the stability of an **Idea** and whether or not it will continue to exist.

If no status is explicitly mentioned, an **Idea** is stable.

The table below describes all statuses and whether or not they have bugs, their age, persistence, and other details.

**Ideas** must transition from one status to another, and potential successors and predecessors to a given status (what
it can transition into and what it transitioned from) are also listed in this table.

| Name |  Maintained | Bugs | Age | Persistent | Predecessors | Successors |
| :--- |:----------- | :--- | :-- | :--------- | :----------- | :--------- |
| **EXPERIMENTAL** (*EX*) | Yes | Maybe | New | TBD | None | S, US, D |
| **STABLE** (*S*) | Yes | Non-fatal | Recent-Old | Yes | EX, US | US, L |
| **UNSTABLE** (*US*) | Yes | Yes, fatal | Recent-Old | Yes | S | S, D |
| **LEGACY** (*L*) | Maybe | Non-fatal | Old | Yes | S | D |
| **DEPRECATED** (*D*) | No | Varied | Varied | No | EX, US, L | None |

Legacy **Ideas** cannot have fatal bugs. If they do, they should be instead marked as UNSTABLE or DEPRECATED.

The general transitions between **Ideas** goes as follows:

**Ideas** are introduced as EXPERIMENTAL. They then become UNSTABLE once it is decided that they will not be removed.
Then, they gradually stabilize and become STABLE. Most **Ideas** will stay here, but some will gradually go "out
of fashion", and become LEGACY, meaning that they will be supported, but are subject to deprecation, and should
not be used unless necessary.

Some **Ideas** will be removed, which will be discussed [later on](#how-do-i-deprecate-ideas)

## How do I name **Ideas**?

**Huge Ideas** should be given noun phrases, such as "Locking", or "Locking Philosophy".

**Big Ideas** should be given component names, such as "Turnstiles".

**Small Ideas** should be given names of functions or behaviors, such as "Turnstile Blocking".

## How should I write **Ideas**?

### **Ideas** should be written in standard markdown in code comments

**Ideas** should be written in an active voice, without unnecessary jargon. 

Referring to the reader with "you", or the author/speaker as "I" is permissible for **Ideas**, 
particularly **Big** and **Small** ones that are more "inward facing" (less about theory, more about implementation). 
However, try to keep the **Idea** more like a piece of documentation and less like a text message.

Unicode diagrams can be embedded in **Ideas** if needed.

In the text of an **Idea**, CAPITAL LETTERS or `*`asterisks`*` can be used to emphasize certain points.

In **Ideas**, code should be frequently referred to. In particular, structures and functions should be referred to.

Pseudocode in **Ideas** is permitted, but don't be bureaucratic about the format of pseudocode. The following are
examples of pseudocode that is acceptable:

```rs
if (condition) {
    do_something()
}
```

```py
while condition is not true
    do things
    check stuff
```

## How big should **Ideas** be?

**Small Ideas** should be the smallest **Ideas**, whereas **Huge Ideas** should be the second smallest, and **Big
Ideas** should be the largest in length.

**Small Ideas** should be under 20 lines.

**Huge Ideas** should be between 50-150 lines.

**Big Ideas** should be between 100-300 lines.

## What about diagrams?

Diagrams are useful, but do not overuse them. Diagrams can be used to describe the parts of a large structure and
interactions between them, but only when necessary to show connections between parts.

## Are there any examples of **Ideas**?

Below are mock examples of **Ideas**. The text in **Sections** are meant to give an overview of how the Sections should be written.

You can find more **Ideas** throughout code to give you a better sense of how they are integrated into the codebase.

**Huge Ideas** belong in separate `.md` files, and do not have a Signature.

```c
/*
 * # Huge Idea: Locking Philosophy
 *
 * # Credits: Sally Mutex
 *
 * # Audience: Everyone
 *
 * # Overview: Locking allows for safe access of shared objects on multitasking kernels...
 *
 * # Background: This is how other operating systems use locks and a bit of history...
 *
 * # Summary: Locking is not a magic wand for instant scalability. A few main types of locks and uses (see struct mutex)...
 *
 * # Interactions: Locking is used almost everywhere that requires shared ownership[^1]...
 *
 * # Constraints: Locking on an SMP preemptible kernel introduces a few problems...
 *
 * # Errors: Deadlocks are a problem and we don't attempt to recover, just report them...
 *
 * # Rationale: We use turnstiles to have pointer sized adaptive mutexes...
 *
 * # References: [^1]: "Name of Idea" `./optional/path` 
 *
 * # Changelog:
 *   09/05/2005 - Sally Mutex: Added information about rwlocks (commit 3a5b9)
 *   09/01/2005 - Sally Mutex: Created Idea
 *
 * # Notes: <Link to Solaris internals book> you can read more about Solaris, which has similar locking philosophy, here.
 *
 */
```

```c
/* @idea:big Turnstiles */
/*
 * # Big Idea: Turnstiles (EXPERIMENTAL)
 *
 * # Alerts: This is still EXPERIMENTAL. Be wary of bugs that may be from this component.
 *
 * # Credits: Eleanor Semaphore
 *
 * # Audience: Synchronization subsystem authors and others interested. not necessary to read.
 *
 * # Overview: Turnstiles give us pointer sized adaptive mutexes (see "Locking Philosophy" [^1])...
 *
 * # Background: Turnstiles were invented by Solaris, and are used in FreeBSD and XNU...
 *
 * # Summary: Turnstiles give us a unified structure with functionalities... this functionality is provided by turnstile_block()...
 *
 * # API: Turnstiles expose these functions, use them like such...
 *
 * # Interactions: Turnstiles are used in our mutex implementation and are not to be used on their own outside of tests...
 *
 * # Constraints: Turnstiles must be efficient and avoid taking the slow blocking path too frequently...
 *
 * # Internals: Turnstiles internally use x, y, and z...
 *
 * # Errors: Turnstiles don't "fail", but these things can...
 *
 * # Rationale: Turnstiles spin when the owner is running because it avoids a slowpath...
 *
 * # Diagrams:
 *
 * -- note: diagrams should have the 3 grave stones preceding and following them. this is omitted
 * here because this document is also in markdown and that would interfere with this.
 *
 *                  Diagram A: Turnstile Donation
 *
 *              turnstile       ┌────────────┐      no existing
 *              ┌─exists────────│   Block    │───────turnstile┐
 *              │               └────────────┘                │
 *              │                                             │
 *              │                                             │
 *              │                                             │
 *              ▼                                             ▼
 *  ┌─────────────────────────┐                      ┌──────────────────┐
 *  │Add Turnstile to freelist│                      │ Donate Turnstile │
 *  └─────────────────────────┘                      └──────────────────┘
 *
 * # Bugs:
 *   #44 "missed wakeup"
 *
 *
 * # Tests:
 *   `./kernel/tests/turnstile.c` - general turnstile tests
 *   `./kernel/tests/mutex.c` - general mutex tests that use turnstiles
 *
 * # References: [^1] "Locking Philosophy" `./docs/locking_idea.md`
 *
 * # Changelog:
 *   09/02/2005 - Eleanor Semaphore: Added second queue for rwlocks (commit 0b4e)
 *   09/01/2005 - Eleanor Semaphore: Created Idea (commit 62ef)
 *
 * # Notes:
 *   Here is some stuff you might be interested in reading regarding the history of turnstiles
 *
 */
```

```c
/* @idea:small Turnstile Blocking */
/*
 * # Small Idea: Turnstile Blocking
 *
 * # Credits: Eleanor Semaphore
 *
 * # Context: This is the blocking portion of the Turnstile implementation. See "Turnstiles" [^1]...
 *
 * # Problem: We aim to solve the issue regarding how threads block on the turnstile and orderings for that...
 *
 * # Strategy: We increment the waiter count in the block path, but decrement it from the owner unblocking a thread...
 *
 * # References: [^1] "Turnstiles" `./kernel/sync/turnstile.c`
 *
 * # Changelog:
 *    09/01/2005 - Eleanor Semaphore: Created Idea (commit 62ef)
 *
 * // notes section not present
 *
 */
```

## How do I refer to other **Ideas**?

Referring to other **Ideas** can be done in a format like this:

In text, make references such as

```c
/*
 * Text [^1].
 */
```

Then, in the **References** **Section** of the **Idea**, provide a source.
```c
/*
 * [^1]: "Name"  `./optional/relative/path/to/idea/from/project/root`
 */
```

### Section 1: "**The Huge Idea**"

**Huge Ideas** are meant to be very abstract. They describe design philosophies and concepts.

Note that **Huge Ideas** go in separate `.md` files and thus do not carry a signature.

The layout for a **Huge Idea** is as follows:

```c
/*
 * # Huge Idea: Name of Idea (optional status)
 *
 * # (optional) Alerts: Short message regarding anything related to this Idea's status.
 *
 * # (optional) Credits: Who wrote this Idea?
 *
 * # Audience: Who is meant to see this?
 *
 * # Overview:
 *   This Huge Idea has a small, succinct overview that describes in 1-2
 *   sentences what the Idea is about.
 *
 * # Background:
 *   This Huge Idea requires some prior background knowledge, which will be discussed here.
 *   Background knowledge is general knowledge that is not just specific to this codebase.
 *
 * # Summary:
 *   This describes the various features that this Idea aims to provide,
 *   and the high level goals that it seeks to accomplish, and also not accomplish.
 *
 * # Interactions:
 *   This explains how this Idea interacts with other Ideas, whether they are
 *   Huge Ideas, Big Ideas, or otherwise. It aims to give context surrounding the
 *   Idea by detailing what and how it interacts with other Ideas.
 *   Context knowledge is context specific to this codebase.
 *
 * # Constraints:
 *   What other Ideas and things prevent this Idea from doing certain things? how
 *   are they constraining this Idea? Do we have workarounds? (e.g. this Idea must
 *   be fast and so we do X, Y, and Z to maximize speed)
 *
 * # Errors:
 *   What potential issues can arise from this Idea and how do we plan to recover/avoid them?
 *
 * # Rationale:
 *   Why were specific choices made that were brought up earlier (use this to go in depth)?
 *
 * # (optional) References:
 *   Other Ideas to reference and look at related to this.
 *
 * # (optional) Changelog:
 *   Major changes to this Idea and dates for the changes.
 *
 * # (optional) Notes:
 *   Other things that could not fit into the other Sections of the Idea
 *
 */
```

**Huge Ideas** should be more focused on *theory* and *interaction*, less on *implementation*. **Huge Ideas** serve to
"paint a picture", not write a paper. **Huge Ideas** should be *shorter* than **Big Ideas**.

### Section 2: "**The Big Idea**"

**Big Ideas** are meant to be less abstract than **Huge Ideas**. Whereas **Huge Ideas** typically discuss the design
philosophy of a subsystem or component, **Big Ideas** should focus on the implementations and private interactions of
components of a **Huge Idea**.

**Big Ideas** should be the largest out of any **Idea**, and should provide thorough information about a component of a
subsystem.

**Big Ideas** share many similar **Section** names to **Huge Ideas**, but the contents of these **Sections** are different. **Big Idea** parts should focus on specific implementation notes and strategies, whereas **Huge Idea** **Sections** are more focused on philosophy and high level motivations.

The layout for a **Big Idea** is as follows:

```c
/* @idea:big Name of Idea */
/*
 * # Big Idea: Name of Idea (optional status)
 *
 * # (optional) Alerts: Short message regarding anything related to this Idea's status.
 *
 * # (optional) Credits: Who wrote this Idea?
 *
 * # Audience: Who is meant to see this?
 *
 * # Overview:
 *   This Big Idea has a small, succinct overview that describes in 1-2
 *   sentences what the Idea is about.
 *
 * # Background:
 *   This Big Idea requires some prior background knowledge, which will be discussed here.
 *   Background knowledge is general knowledge that is not just specific to this codebase.
 *
 * # Summary:
 *   This describes the various features that this Idea aims to provide,
 *   and the high level goals that it seeks to accomplish, and also not accomplish.
 *
 * # API:
 *   This goes further into detail than Features alone. It describes functions and structures
 *   that the "outside world" is allowed to use, and how they are provided by this Idea, as well
 *   as the use cases for such functions and structures outside of the scope of this Idea. Potential
 *   errors are also detailed here for each function, but are expanded upon in the next Section.
 *
 * # Errors:
 *   What potential issues can arise from this Idea and how do we plan to recover/avoid them?
 *
 * # Context:
 *   Similar to "Interactions" from Huge Ideas, the Context of a Big Idea describes the Huge Idea(s)
 *   it resides beneath, and the Small Ideas that reside beneath it. It should not go
 *   too far away and start discussing other Huge Ideas unless necessary.
 *   Context knowledge is context specific to this codebase.
 *
 * # Constraints:
 *   What is preventing this Idea from doing certain things, and how are we able to work around them?
 *
 * # Internals:
 *   What are concerns that people working on this should have? Things like locking, memory ordering,
 *   and handling preemption should be discussed here. This is also a place where authors can create
 *   extra Sections, such as "Internals - lock ordering". Pitfalls and other weird things can
 *   be talked about in the Internals.
 *
 * # Strategy:
 *   Specifically what steps are we taking to achieve the goals we outlined earlier? What do we need
 *   for those goals to come to fruition? How are they accomplished internally?
 *   (e.g., why did we pick X instead of Y?)
 *
 * # Rationale:
 *   Why were specific choices made that were brought up earlier (use this to go in depth)?
 *
 * # (optional) Bugs:
 *   Any bugs related to this Idea?
 *
 * # (optional) Tests:
 *   What tests are related to this Idea?
 *
 * # (optional) References:
 *   Other Ideas to reference and look at related to this.
 *
 * # (optional) Changelog:
 *   Major changes to this Idea and dates for the changes.
 *
 * # (optional) Notes:
 *   Other things that could not fit into the other Sections of the Idea
 *
 */
```

**Big Ideas** should seek to thoroughly describe a component, and it is not unexpected for a **Big Idea** to be so
thorough that **Small Ideas** are not necessary. However, **Big Ideas** can reference **Small Ideas** beneath them.

### Section 3: "**The Small Idea**"

**Small Ideas** should describe singular functions or sets of functions. **Small Ideas** are used to discuss specific
pitfalls regarding functions, and exact details of strategies.

**Small Ideas** are internal. They are not for the outside world to see and read, and thus, many **Sections** of other
**Ideas** are not present.

The layout for a **Small Idea** is as follows:

```c
/* @idea:small Name of Idea */
/*
 * # Small Idea: Name of Idea (optional status)
 *
 * # (optional) Alerts: Short message regarding anything related to this Idea's status.
 *
 * # (optional) Credits: Who wrote this Idea?
 *
 * # Context:
 *   Which Ideas does this reside under?
 *
 * # Problem:
 *   Specifically which piece of the problem are we trying to solve?
 *
 * -- note: external APIs should've already been discussed at this point. we are looking at a single function
 *
 * # Strategy:
 *   Exactly what are we doing to resolve this problem? What other Ideas is this interacting with and how?
 *   This Section can be merged with the Problem part if the Problem part adequately covers this Section.
 *
 *
 *
 * # (optional) References:
 *   Other Ideas to reference and look at related to this Idea.
 *
 * # (optional) Changelog:
 *   Major changes to this Idea and dates for the changes.
 *
 * # (optional) Notes:
 *   Other things that could not fit into the other Sections of the Idea
 *
 */
```

# How do I update things?

## How often and how should I update **Ideas**?

Typically, the smaller the **Idea**, the more frequently it should be updated. **Small Ideas** should be be updated
whenever the functionality of the function or a set of functions it encompasses changes. **Big Ideas** should be changed
whenever the component they describe has a major API change or a major internal detail change, and **Huge Ideas** should
only be changed if there is a complete overhaul in design philosophy.

Changes for **Ideas** should be kept in the **Changelog** **Section** of the **Idea**, in a format like this:

```c
/*
 * Date - Author: Description: (commit hash, optional)
 */
```

For example:

```c
/* # Changelog:
 *   11/12/1997 - Bumblebee: Added ham to the sandwich recipe (commit a4f0e19)
 *   02/13/1994 - Raven: Changed toaster model (commit c4e814)
 *
 */
```

## How do I deprecate **Ideas**?

Because **Ideas** each contain a status, the status should be updated to depcrecate **Ideas**. When an **Idea** becomes
DEPRECATED, its code will still be present, but will issue a warning whenever it is executed. Once all code no longer
has a piece of DEPRECATED code, it will be removed. 

If an **Idea** should still be supported (e.g., things like Linux's semaphores), then mark the **Idea** as LEGACY, but
include a note that an **Idea** has better alternatives in newer code. 
