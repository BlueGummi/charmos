#!/bin/bash

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <log_prefix>"
    exit 1
fi

LOG_PREFIX=$1

cat << EOF
LOG_SITE_EXTERN($LOG_PREFIX);
LOG_HANDLE_EXTERN($LOG_PREFIX);

#define ${LOG_PREFIX}_log(lvl, fmt, ...) \\
    log(LOG_SITE($LOG_PREFIX), LOG_HANDLE($LOG_PREFIX), lvl, fmt, ##__VA_ARGS__)

#define ${LOG_PREFIX}_err(fmt, ...)   ${LOG_PREFIX}_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define ${LOG_PREFIX}_warn(fmt, ...)  ${LOG_PREFIX}_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define ${LOG_PREFIX}_info(fmt, ...)  ${LOG_PREFIX}_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define ${LOG_PREFIX}_debug(fmt, ...) ${LOG_PREFIX}_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define ${LOG_PREFIX}_trace(fmt, ...) ${LOG_PREFIX}_log(LOG_TRACE, fmt, ##__VA_ARGS__)
EOF
