#pragma once
#include <test/test.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types/types.h>

TEST_GROUP_DEFINE(ui128);

uint128_t __ashlti3(uint128_t a, int b);
uint128_t __lshrti3(uint128_t a, int b);
int128_t __ashrti3(int128_t a, int b);
int128_t __negti2(int128_t a);
int128_t __multi3(int128_t a, int128_t b);
uint128_t __udivmodti4(uint128_t n, uint128_t d, uint128_t *rem);
uint128_t __udivti3(uint128_t a, uint128_t b);
uint128_t __umodti3(uint128_t a, uint128_t b);
int128_t __divmodti4(int128_t a, int128_t b, int128_t *rem);
int128_t __divti3(int128_t a, int128_t b);
int128_t __modti3(int128_t a, int128_t b);
int __clzti2(uint128_t a);
int __ctzti2(uint128_t a);
int __ffsti2(int128_t a);
int __popcountti2(uint128_t a);
int __parityti2(uint128_t a);
