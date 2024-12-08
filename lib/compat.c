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

char * wsa_strerror(int wsa_errno) {
  switch (wsa_errno) {
  case 10014:
    return "(WSA)EFAULT";
  case 10022:
    return "(WSA)EINVAL";
  case 10035:
    return "(WSA)EWOULDBLOCK";
  case 10050:
    return "(WSA)ENETDOWN";
  case 10052:
    return "(WSA)ENETRESET";
  case 10053:
    return "(WSA)ECONNABORTED";
  case 10054:
    return "(WSA)ECONNRESET";
  default:
    break;
  }
  return "(see winsock2.h)";
}
#endif
