/* @title: Rounding up and Down */
#pragma once
#define ROUND_UP(x, y) (((x) + (y) - 1) / (y))
#define ROUND_DOWN(x, y) ((x) / (y))
#define ROUND(x, y) (((x) + ((y) / 2)) / (y))
#define ROUND_UP_TO_POWER_OF_2(x) (1 << (32 - __builtin_clz(x - 1)))
#define ROUND_DOWN_TO_POWER_OF_2(x) (1 << (31 - __builtin_clz(x)))
#define ROUND_TO_POWER_OF_2(x) (1 << (32 - __
