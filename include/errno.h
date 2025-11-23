/* @title: Error Codes */
#pragma once
#include <stdint.h>

#define ERR_IS_FATAL(e) (e != ERR_OK && e != ERR_AGAIN)

enum errno : int32_t {
    ERR_OK = 0,          // Success
    ERR_UNKNOWN = -1,    // Unknown or unspecified error
    ERR_NO_MEM = -2,     // Out of memory
    ERR_NO_DEV = -3,     // No such device
    ERR_NO_ENT = -4,     // No such file or directory
    ERR_EXIST = -5,      // File already exists
    ERR_IO = -6,         // I/O error
    ERR_NOT_DIR = -7,    // Not a directory
    ERR_IS_DIR = -8,     // Is a directory
    ERR_INVAL = -9,      // Invalid argument
    ERR_PERM = -10,      // Permission denied
    ERR_FAULT = -11,     // Bad address
    ERR_BUSY = -12,      // Resource/device busy
    ERR_AGAIN = -13,     // Try again later
    ERR_NOT_IMPL = -14,  // Not implemented
    ERR_NOSPC = -15,     // No space left on device
    ERR_OVERFLOW = -16,  // Value too large
    ERR_NOT_EMPTY = -17, // Directory not empty

    ERR_FS_NO_INODE = -100,     // Inode not found
    ERR_FS_CORRUPT = -101,      // Filesystem corruption
    ERR_FS_SYMLINK_LOOP = -102, // Too many symlink levels
    ERR_FS_INTERNAL = -103,     // Internal filesystem error

};

static inline const char *errno_to_str(enum errno err) {
    switch (err) {
    case ERR_OK: return "No error";
    case ERR_UNKNOWN: return "Unknown error";
    case ERR_NO_MEM: return "Out of memory";
    case ERR_NO_DEV: return "No such device";
    case ERR_NO_ENT: return "No such file or directory";
    case ERR_EXIST: return "File already exists";
    case ERR_IO: return "I/O error";
    case ERR_NOT_DIR: return "Not a directory";
    case ERR_IS_DIR: return "Is a directory";
    case ERR_INVAL: return "Invalid argument";
    case ERR_PERM: return "Permission denied";
    case ERR_FAULT: return "Bad memory access";
    case ERR_BUSY: return "Resource busy";
    case ERR_AGAIN: return "Try again";
    case ERR_NOT_IMPL: return "Not implemented";
    case ERR_NOSPC: return "No space left";
    case ERR_OVERFLOW: return "Value too large";

    case ERR_FS_NO_INODE: return "Inode not found";
    case ERR_FS_CORRUPT: return "Filesystem corruption";
    case ERR_FS_SYMLINK_LOOP: return "Symlink loop";

    default: return "Unrecognized error code";
    }
}
