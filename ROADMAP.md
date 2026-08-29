# Where are we going?

Since a lot of the development isn't necessarily a long sequential
set of steps, this roadmap is structured as lanes. Each lane
has a start condition (i.e. something that unblocks it), and
when the start condition is met, it can proceed all the way
to the end, unless something comes up. Start conditions
by default are treated as unmet, and when they are met,
they are annotated as such.

Start conditions are not necessary, lanes without them are treated
as having no start condition.

Lanes are marked as "Unsketched/Sketched/In Progress/Completed", and I will
hopefully remember to update this file so that it remains accurate.

Unsketched/Sketched is about whether the lane has discrete, actionable steps
and steps to follow through on, and steps that are changed post-hoc
are marked as such with a note. A sketched lane can still not have its
start condition met.

There is also a general track of "the road to userspace" so things are
marked as main/middle/side to distinguish what is actually on that road
to getting there, and what is a side quest/QOL.

Lanes are composed of items, and the syntax convention here is

```
N.2
```

Where `N` indicates the lane, `2` indicates the step.

Lanes themselves can also have numbers in the LHS, such as `SCH1`,
`SCH2`, to mark a different lane under a component.

Anyways, enough talk...

# Roadmap

## `NIGHTMARE1` - Wiring up nightmare tests in CI (NEAR **SKETCHED** MAIN)

The title. Just need to finalize a few more things and get it up.

- [ ] 1. Test things
- [ ] 2. Bring up GUI frontend
- [ ] 3. Introduce real test consumers

## `SYNC1` - Wiring up qspinlock as the default spinlock, implementing more scalable synchronization primitives (in line with RCU) (NEAR **UNSKETCHED** MAIN)

The title. We have qspinlock but default spinlock doesn't route through it, would be nice to do that, and
also RCU falls right in line with this, along with maybe exploring some other stuff

## `FS1` - Wiring up the folio and other mm features for filesystem overhaul beginnings (NEAR **UNSKETCHED** MAIN)

Not too sure on the specifics just yet, we'll need to plan it out

> Start condition: Better synchronization primitives, percpu refcounting usage, maybe CNA/lock cohorting, etc. 

## `MEM1` - Improving some allocators (NEAR **SKETCHED** SIDE)

Some allocators are a little funny, especially with address space sharding on UMA.
Just a few things to fix up, no big deal, not really correctness.

- [ ] 1. Slab
- [ ] 2. Buddy
- [ ] 3. Update where relevant

## `RCU1` - Scaling RCU and making it generally better (NEAR **SKETCHED** MAIN)

Basically the title, RCU is quite bad, and I had held off on making it better
because it's notoriously hard, especially with edge cases. Now that we have
testing stabilizing more, I'm open to opening this up.

> Start condition: nightmare test stabilization

- [ ] 1. Look at what linux does
- [ ] 2. Tree RCU
- [ ] 3. More RCU configs

## `FAKEDEV1` - Fake Devices for Testing (NEAR **SKETCHED** MAIN)

Implementing fake devices that can fuzz drivers
and find niche unhandled error conditions (what was it, Linux
was found to have 3-7x more bugs in USB, primarily in error cases (?))

- [ ] 1. API for fake device registers
- [ ] 2. Tooling component
- [ ] 3. Fake device wrappers and functions + structures
- [ ] 4. Actually implement one and test one
- [ ] 5. Solidfy this somewhere

## `BUILD1` - Build System Improvements (NEAR **SKETCHED** MAIN)

Making the build system a little nicer,
so as to allow us to scale it up more in the future with better ways
of organizing feature flags and naming conventions

- [ ] 1. Come up with better naming scheme
- [ ] 2. Move and update files
- [ ] 3. Record it somewhere

## `CONFIG1` - Making more configuration options available (FURTHER **UNSKETCHED** SIDE)

The kernel only has a few, things like CONFIG_SMP and CONFIG_PREEMPT and a bunch
of other stuff would be great, but not necessary

> Start condition: must look into devising more flags and features

## `TEST1` - Testing On Real Hardware (FURTHER **UNSKETCHED** MAIN)

Giving hardware testing a reliable harness with a database for centralized
reporting and viewing, so we can test releases

> Start condition: tests must exist, drivers, etc. and servers and stuff

## `DOC1` - Documentation (FURTHER **UNSKETCHED** SIDE)

Writing actual documentation as APIs stabilize and stop moving around

> Start condition: APIs must actually stabilize a bit

## `WATCHDOG1` - Watchdog Abstraction Layer (FURTHER **UNSKETCHED** SIDE)

Scaling the watchdog into an abstract interface,
becoming more of a "monitor" with heterogeneous watchdogs that function more
as independent actors, giving flexibility but with some complexities.

> Start condition: we need to get to a point where this is actually
> a necessity for liveness inspection and scalability, and not just bikeshed.

## `ACPI1` - ACPI Extensions and Features (FURTHER **UNSKETCHED** MIDDLE)

Supporting ACPI sleep functions, and it lives a little
upstream of hotplugging functionality and whatnot.

> Start condition: we need to actually get ACPI tables from real
> machines to test with this (trivial), so this is technically unblocked
> but low priority.

## `RELEASE1` - First Release (FAR **UNSKETCHED** MAIN)

It might happen

> Start condition: unsettled

# Completed Items

:cricket: :cricket: :cricket:
