/* @title: APC Types */
#pragma once

enum apc_type {
    APC_TYPE_SPECIAL_KERNEL,
    APC_TYPE_KERNEL,
    APC_TYPE_USER,
    APC_TYPE_BOUNDARY,
    APC_TYPE_COUNT
};

struct apc_queue {
    struct apc *head;
    struct apc *tail;
};

struct apc;
struct apc_event_desc;

typedef void (*apc_func_t)(struct apc *apc);
