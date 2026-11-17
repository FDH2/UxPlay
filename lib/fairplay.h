/**
 *  Copyright (C) 2018  Juho Vähä-Herttua
 *  Copyright (C) 2026  Omar Roth
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
 */

#ifndef FAIRPLAY_H
#define FAIRPLAY_H

#include "logger.h"

typedef struct fairplay_s fairplay_t;

fairplay_t *fairplay_init(logger_t *logger);
int fairplay_setup(fairplay_t *fp, const unsigned char req[16], unsigned char res[142]);
int fairplay_handshake(fairplay_t *fp, const unsigned char req[164], unsigned char res[32]);
int fairplay_decrypt(fairplay_t *fp, const unsigned char input[72], unsigned char output[16]);
unsigned int fairplay_get_version(const fairplay_t *fp);
void fairplay_destroy(fairplay_t *fp);

#endif
