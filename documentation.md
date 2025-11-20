# Documentation Guide

Similar to the style guide, this is a documentation *guide*, not a set of documentation *rules*. There may be instances where documentation may deviate from this guide, and that is expected and allowed, but try to keep most things to these guidelines.

## "The Format"

Documentation for this codebase should be written in a unified format. This format will be referred to as the "Idea Structure".

The description, rationale, and examples of the Idea Structure will be laid out and explained below.

### What is the Idea Structure?

Code can almost always be sorted into different components. These components can come together and make larger components, which can continue to create even larger pieces.

Unifying a format for describing and documenting code helps provide codebase consistency, makes documentation predictable, and offers a useful checklist of items to reduce the chance that the documentation misses something.

The Idea Structure defines "Ideas" of different "Sizes". As the Size of an Idea increases, the scope that it covers increases, but the granularity and specificity of the Idea decreases. Ideas can refer to each other.

There are three main Sizes of Ideas, each having their own use case:

- Huge Idea: describes entire concepts (e.g. the memory allocator design philosophy)
- Big Idea: describes one significant component of a subsystem (e.g. the slab allocator)
- Small Idea: describes sets of functions or singular functions of a subsystem (e.g. the garbage collector main loop)

### What is the format of an Idea?

The different pieces of an Idea will be referred to as "Parts".

Ideas follow a common format, but at each Size, Parts of the Idea are added and removed. 

In general, each Idea MUST include the following Parts, unless explicitly specified otherwise:

- Name: what is this Idea?
- Overview/problem: one or two sentences that describe in simple language what the Idea is about
- Audience: who is supposed to read this Idea?
- Status: is the idea experimental, stable, or legacy?

Ideas MAY include these Parts, optionally:
- Date + credits: when and whom wrote this Idea, and when was it updated?
- References: other Ideas mentioned
- Notes: other things not covered by other Parts of the Idea

The following sections will define the different Parts of Ideas of different Sizes.

#### Section 1: "The Huge Idea"

Huge Ideas are meant to be very abstract. They describe design philosophies and concepts.

The layout for a Huge Idea is as follows:

```c
/*
 * Name: Name of Idea
 *
 * Credits: Who wrote this Idea, when was it updated, and when was it created? (optional)
 *
 * Audience: Who is meant to see this?
 *
 * Status: Status of the idea
 *
 * Overview:
 *   This Huge Idea has a small, succint overview that describes in 1-2 
 *   sentences what the Idea is about.
 *
 * Feature Summary + Purpose + non-goals:
 *   This describes the various features that this Idea aims to provide,
 *   and the high level goals that it seeks to accomplish, and also not accomplish.
 *
 * Interactions:
 *   This explains how this Idea interacts with other Ideas, whether they are
 *   Huge Ideas, Big Ideas, or otherwise. It aims to give context surrounding the
 *   Idea by detailing what and how it interacts with other Ideas.
 *
 * Constraints:
 *   What other Ideas and things prevent this Idea from doing certain things? how
 *   are they constraining this Idea? Do we have workarounds? (e.g. this Idea must
 *   be fast and so we do X, Y, and Z to maximize speed)
 * 
 * Errors and error recovery:
 *   What potential issues can arise from this Idea and how do we plan to recover/avoid them?
 *
 * Rationale and Motivation:
 *   Why were specific choices made that were brought up earlier (use this to go in depth)?
 *
 * References:
 *   Other Ideas to reference and look at related to this.
 *
 * Notes:
 *   Other things that could not fit into the other Parts of the Idea
 *
 */
```

Huge Ideas should be more focused on theory and interaction, less on implementation. Huge Ideas serve to "paint a picture", not write a paper. Huge Ideas should be expected to be shorter than Big Ideas.

#### Section 2: "The Big Idea"

Big Ideas are meant to be less abstract than Huge Ideas. Whereas Huge Ideas typically encompass whole subsystems and huge API descriptions, Big Ideas should be scoped to a smaller directory or a file. They focus on the implementations and private interactions of components of a Huge Idea. 

Big Ideas should be the largest out of any Idea, and should provide thorough information about a component of a subsystem.

The layout for a Big Idea is as follows:

```c
/*
 * Name: Name of Idea
 *
 * Credits: Who wrote this Idea, when was it updated, and when was it created? (optional)
 *
 * Audience: Who is meant to see this?
 *
 * Status: Status of the idea
 *
 * Overview:
 *   This Big Idea has a small, succint overview that describes in 1-2 
 *   sentences what the Idea is about.
 *
 * Feature Summary + Purpose + non-goals:
 *   This describes the various features that this Idea aims to provide,
 *   and the high level goals that it seeks to accomplish, and also not accomplish.
 *
 * External API and uses:
 *   This goes further into detail than Features alone. It describes functions and structures
 *   that the "outside world" is allowed to use, and how they are provided by this Idea, as well
 *   as the use cases for such functions and structures outside of the scope of this Idea. Potential
 *   errors are also detailed here for each function, but are expanded upon in the next Part.
 *
 * Errors and error recovery:
 *   What potential issues can arise from this Idea and how do we plan to recover/avoid them?
 *
 * Context:
 *   Similar to "Interactions" from Huge Ideas, the Context of a Big Idea describes the Huge Idea(s)
 *   it resides beneath, and the Small Ideas that reside beneath it. It should not go
 *   too far away and start discussing other Huge Ideas unless necessary.
 *
 * Constraints and Invariants:
 *   What is preventing this Idea from doing certain things, and how are we able to work around them?
 *
 * Internal Details:
 *   What are concerns that people working on this should have? Things like locking, memory ordering,
 *   and handling preemption should be discussed here. This is also a place where authors can create 
 *   extra Parts, such as "Internal Detail - lock ordering". Pitfalls and other weird things can
 *   be talked about in these Internal Details.
 *
 * Strategy:
 *   Specifically what steps are we taking to achieve the goals we outlined earlier? What do we need
 *   for those goals to come to fruition? How are they accomplished internally? 
 *   (e.g., why did we pick X instead of Y?)
 *
 * Rationale and Motivation:
 *   Why were specific choices made that were brought up earlier (use this to go in depth)?
 *
 * References:
 *   Other Ideas to reference and look at related to this.
 *
 * Notes:
 *  Other things that could not fit into the other Parts of the Idea
 *
 */
```

Big Ideas should seek to thoroughly encompass a component, and it is not unexpected for a Big Idea to be so thorough that smaller Ideas are not necessary. However, Big Ideas can reference smaller Ideas beneath them.

#### Section 3: "The Small Idea"

Small Ideas should describe singular functions or sets of functions. Small Ideas are used to discuss specific pitfalls regarding functions, and exact details of strategies.

Small Ideas are internal. They are not for the outside world to see and read, and thus, many Parts of other Ideas are not present.

The layout for a Small Idea is as follows:

```c
/*
 * Name: Name of Idea
 *
 * Credits: Who wrote this Idea, when was it updated, and when was it created? (optional)
 *
 * Context:
 *   Which Ideas does this reside under? What is the visibility?
 *
 * Problem:
 *   Specifically which piece of the problem are we trying to solve?
 *
 * # note: external APIs should've already been discussed at this point. we are looking at a single function
 *
 * Strategy and Details and Invariants:
 *   Exactly what are we doing to resolve this problem? What other Ideas is this interacting with and how?
 *
 * Notes:
 *  Other things that could not fit into the other Parts of the Idea
 *
 */
```
