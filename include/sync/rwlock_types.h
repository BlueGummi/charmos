/* @title: Reader writer lock types */
#pragma once

struct rwlock;

/* Putting this here for wroost.h */
enum rwlock_acquire_type {
    RWLOCK_READ = 0,
    RWLOCK_WRITE = 1,
};
