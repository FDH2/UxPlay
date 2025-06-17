
/*
 * Copyright (c) 2024 F. Duncanh, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *==================================================================
 */

#ifdef _WIN32
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#define MAX_SOCKET_ERROR_MESSAGE_LENGTH 256

/* Windows (winsock2) socket error message text */
char *wsa_strerror(int errnum) {
    static char message[MAX_SOCKET_ERROR_MESSAGE_LENGTH] = { 0 };
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
                  0, errnum, 0, message, sizeof(message), 0);
    char *nl = strchr(message, '\n');
    if (nl) {
        *nl = 0;  /* remove any trailing newline, or truncate to one line */
    }
    return message;
}
#endif
