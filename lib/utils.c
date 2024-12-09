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
 *=================================================================
 * modified by fduncanh 2021-2022
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <stdint.h>
#define SECOND_IN_NSECS 1000000000UL

char *
utils_strsep(char **stringp, const char *delim)
{
    char *original;
    char *strptr;

    if (*stringp == NULL) {
        return NULL;
    }

    original = *stringp;
    strptr = strstr(*stringp, delim);
    if (strptr == NULL) {
        *stringp = NULL;
        return original;
    }
    *strptr = '\0';
    *stringp = strptr+strlen(delim);
    return original;
}

int
utils_read_file(char **dst, const char *filename)
{
    FILE *stream;
    int filesize;
    char *buffer;
    int read_bytes;

    /* Open stream for reading */
    stream = fopen(filename, "rb");
    if (!stream) {
        return -1;
    }

    /* Find out file size */
    fseek(stream, 0, SEEK_END);
    filesize = ftell(stream);
    fseek(stream, 0, SEEK_SET);

    /* Allocate one extra byte for zero */
    buffer = malloc(filesize+1);
    if (!buffer) {
        fclose(stream);
        return -2;
    }

    /* Read data in a loop to buffer */
    read_bytes = 0;
    do {
        int ret = fread(buffer+read_bytes, 1,
                        filesize-read_bytes, stream);
        if (ret == 0) {
            break;
        }
        read_bytes += ret;
    } while (read_bytes < filesize);

    /* Add final null byte and close stream */
    buffer[read_bytes] = '\0';
    fclose(stream);

    /* If read didn't finish, return error */
    if (read_bytes != filesize) {
        free(buffer);
        return -3;
    }

    /* Return buffer */
    *dst = buffer;
    return filesize;
}

int
utils_hwaddr_raop(char *str, int strlen, const char *hwaddr, int hwaddrlen)
{
    int i,j;

    /* Check that our string is long enough */
    if (strlen == 0 || strlen < 2*hwaddrlen+1)
        return -1;

    /* Convert hardware address to hex string */
    for (i=0,j=0; i<hwaddrlen; i++) {
        int hi = (hwaddr[i]>>4) & 0x0f;
        int lo = hwaddr[i] & 0x0f;

        if (hi < 10) str[j++] = '0' + hi;
        else         str[j++] = 'A' + hi-10;
        if (lo < 10) str[j++] = '0' + lo;
        else         str[j++] = 'A' + lo-10;
    }

    /* Add string terminator */
    str[j++] = '\0';
    return j;
}

int
utils_hwaddr_airplay(char *str, int strlen, const char *hwaddr, int hwaddrlen)
{
    int i,j;

    /* Check that our string is long enough */
    if (strlen == 0 || strlen < 2*hwaddrlen+hwaddrlen)
        return -1;

    /* Convert hardware address to hex string */
    for (i=0,j=0; i<hwaddrlen; i++) {
        int hi = (hwaddr[i]>>4) & 0x0f;
        int lo = hwaddr[i] & 0x0f;

        if (hi < 10) str[j++] = '0' + hi;
        else         str[j++] = 'a' + hi-10;
        if (lo < 10) str[j++] = '0' + lo;
        else         str[j++] = 'a' + lo-10;

        str[j++] = ':';
    }

    /* Add string terminator */
    if (j != 0) j--;
    str[j++] = '\0';
    return j;
}

char *utils_parse_hex(const char *str, int str_len, int *data_len) {
    assert(str_len % 2 == 0);

    char *data = malloc(str_len / 2);

    for (int i = 0; i < (str_len / 2); i++) {
        char c_1 = str[i * 2];
        if (c_1 >= 97 && c_1 <= 102) {
            c_1 -= (97 - 10);
        } else if (c_1 >= 65 && c_1 <= 70) {
            c_1 -= (65 - 10);
        } else if (c_1 >= 48 && c_1 <= 57) {
            c_1 -= 48;
        } else {
            free(data);
            return NULL;
        }

        char c_2 = str[(i * 2) + 1];
        if (c_2 >= 97 && c_2 <= 102) {
            c_2 -= (97 - 10);
        } else if (c_2 >= 65 && c_2 <= 70) {
            c_2 -= (65 - 10);
        } else if (c_2 >= 48 && c_2 <= 57) {
            c_2 -= 48;
        } else {
            free(data);
            return NULL;
        }

        data[i] = (c_1 << 4) | c_2;
    }

    *data_len = (str_len / 2);
    return data;
}

char *utils_pk_to_string(const unsigned char *pk, int pk_len) {
    char *pk_str = (char *) malloc(2*pk_len + 1);
    char* pos = pk_str;
    for (int i = 0; i < pk_len; i++) {
        snprintf(pos, 3, "%2.2x", *(pk + i));
        pos +=2;
    }
    return pk_str;
}

char *utils_data_to_string(const unsigned char *data, int datalen, int chars_per_line) {
    assert(datalen >= 0);
    assert(chars_per_line > 0);
    int len = 3*datalen + 1;
    if (datalen > chars_per_line) {
        len += (datalen-1)/chars_per_line;
    }
    char *str = (char *) calloc(len + 1, sizeof(char));
    assert(str);
    char *p = str;
    int n = len + 1;
    for (int i = 0; i < datalen; i++) {
        if (i > 0 && i % chars_per_line == 0) {
            snprintf(p, n, "\n");
            n--;
            p++;
        }
        snprintf(p, n, "%2.2x ", (unsigned int) data[i]);
        n -= 3;
        p += 3;
    }
    snprintf(p, n, "\n");
    n--;
    p++;
    assert(p == &(str[len]));
    assert(len == strlen(str));
    return str;
}

char *utils_data_to_text(const char *data, int datalen) {
    char *ptr = (char *) calloc(datalen + 1, sizeof(char));
    assert(ptr);
    strncpy(ptr, data, datalen);
    char *p = ptr;
    while (p) {
        p  = strchr(p, '\r');  /* replace occurences of '\r' by ' ' */
	if (p) *p = ' ';
    }
    return ptr;
}

void ntp_timestamp_to_time(uint64_t ntp_timestamp, char *timestamp, size_t maxsize) {
    time_t rawtime = (time_t) (ntp_timestamp / SECOND_IN_NSECS);
    struct tm ts = *localtime(&rawtime);
    assert(maxsize > 29);
#ifdef _WIN32  /*modification for compiling for Windows */
    strftime(timestamp, 20, "%Y-%m-%d %H:%M:%S", &ts);
#else
    strftime(timestamp, 20, "%F %T", &ts);
#endif
    snprintf(timestamp + 19, 11,".%9.9lu", (unsigned long) ntp_timestamp % SECOND_IN_NSECS);
}

void ntp_timestamp_to_seconds(uint64_t ntp_timestamp, char *timestamp, size_t maxsize) {
    time_t rawtime = (time_t) (ntp_timestamp / SECOND_IN_NSECS);
    struct tm ts = *localtime(&rawtime);
    assert(maxsize > 12);
    strftime(timestamp, 3, "%S", &ts);
    snprintf(timestamp + 2, 11,".%9.9lu", (unsigned long) ntp_timestamp % SECOND_IN_NSECS);
}

int utils_ipaddress_to_string(int addresslen, const unsigned char *address, unsigned int zone_id, char *string, int sizeof_string) {
    int ret = 0;
    unsigned char ipv6_link_local_prefix[] = { 0xfe, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };
    assert(sizeof_string > 0);
    assert(string);
    if (addresslen != 4 && addresslen != 16) { //invalid address length   (only ipv4 and ipv6 allowed)
        string[0] = '\0';
    }
    if (addresslen == 4) {          /* IPV4 */
        ret = snprintf(string, sizeof_string, "%d.%d.%d.%d", address[0], address[1], address[2], address[3]);
    } else if (zone_id) {           /* IPV6 link-local  */
        if (memcmp(address, ipv6_link_local_prefix, 8)) { 
            string[0] = '\0';     //only link-local ipv6 addresses can have a zone_id
        } else {
	    ret = snprintf(string, sizeof_string, "fe80::%02x%02x:%02x%02x:%02x%02x:%02x%02x%%%u",
                           address[8], address[9], address[10], address[11],
                           address[12], address[13], address[14], address[15], zone_id);
        }
    } else {          /* IPV6 standard*/
        ret = snprintf(string, sizeof_string, "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                       address[0], address[1], address[2], address[3], address[4], address[5], address[6], address[7],
                       address[8], address[9], address[10], address[11], address[12], address[13], address[14], address[15]);
    }
    return ret;
}

const char *gmt_time_string() {
  static char date_buf[64];
  memset(date_buf, 0, 64);

  time_t now = time(0);
  if (strftime(date_buf, 63, "%c GMT", gmtime(&now)))
    return date_buf;
  else
    return "";
}
