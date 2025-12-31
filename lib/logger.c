/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *===============================================================
 * modified by fduncanh 2023-25
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#include "logger.h"
#include "compat.h"

struct logger_s {
    mutex_handle_t lvl_mutex;
    mutex_handle_t cb_mutex;
  
    int level;
    void *cls;
    logger_callback_t callback;
};

logger_t *
logger_init()
{
    logger_t *logger = calloc(1, sizeof(logger_t));
    assert(logger);

    MUTEX_CREATE(logger->lvl_mutex);
    MUTEX_CREATE(logger->cb_mutex);

    logger->level = LOGGER_WARNING;
    logger->callback = NULL;
    return logger;
}

void
logger_destroy(logger_t *logger)
{
    MUTEX_DESTROY(logger->lvl_mutex);
    MUTEX_DESTROY(logger->cb_mutex);
    free(logger);
}

void
logger_set_level(logger_t *logger, int level)
{
    assert(logger);

    MUTEX_LOCK(logger->lvl_mutex);
    logger->level = level;
    MUTEX_UNLOCK(logger->lvl_mutex);
}

int
logger_get_level(logger_t *logger)
{   
    assert(logger);

    MUTEX_LOCK(logger->lvl_mutex);
    int level = logger->level;
    MUTEX_UNLOCK(logger->lvl_mutex);

    return level;
}

void
logger_set_callback(logger_t *logger, logger_callback_t callback, void *cls)
{
    assert(logger);

    MUTEX_LOCK(logger->cb_mutex);
    logger->cls = cls;
    logger->callback = callback;
    MUTEX_UNLOCK(logger->cb_mutex);
}

void
logger_log(logger_t *logger, int level, const char *fmt, ...)
{
    MUTEX_LOCK(logger->lvl_mutex);
    if (level > logger->level) {
        MUTEX_UNLOCK(logger->lvl_mutex);
        return;
    }
    MUTEX_UNLOCK(logger->lvl_mutex);

    char buffer[4096] = {0};
    char err_fmt[] = "---logger message is truncated from %d to %d chars---\n";
    char err_buf[128] = {0};
    va_list ap;
    va_start(ap, fmt);    
    int message_len = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    if (message_len >=  (int) sizeof(buffer)) {
        snprintf(err_buf, sizeof(err_buf), err_fmt, message_len, (int) sizeof(buffer) -1);
    }
    MUTEX_LOCK(logger->cb_mutex);
    assert (logger->callback);
    logger->callback(logger->cls, level, buffer);
    if (err_buf[0]) {
        logger->callback(logger->cls, level, err_buf);
    }
    MUTEX_UNLOCK(logger->cb_mutex);
}
