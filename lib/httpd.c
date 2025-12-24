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
 *===================================================================
 * modified by fduncanh 2022
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <errno.h>

#include "httpd.h"
#include "netutils.h"
#include "http_request.h"
#include "compat.h"
#include "logger.h"
#include "utils.h"

static const char *typename[] = {
    [CONNECTION_TYPE_UNKNOWN] = "Unknown",
    [CONNECTION_TYPE_RAOP]    = "RAOP",
    [CONNECTION_TYPE_AIRPLAY] = "AirPlay",
    [CONNECTION_TYPE_PTTH]    = "AirPlay (reversed)",
    [CONNECTION_TYPE_HLS]     = "HLS"
};

struct http_connection_s {
    int connected;

    int socket_fd;
    void *user_data;
    connection_type_t type;
    http_request_t *request;
};
typedef struct http_connection_s http_connection_t;

struct httpd_s {
    logger_t *logger;
    httpd_callbacks_t callbacks;

    int max_connections;
    int open_connections;
    http_connection_t *connections;
    char nohold;

    /* These variables only edited mutex locked */
    int running;
    int joined;
    thread_handle_t thread;
    mutex_handle_t run_mutex;

    /* Server fds for accepting connections */
    int server_fd4;
    int server_fd6;
};

const char *
httpd_get_connection_typename (connection_type_t type) {
  return typename[type];
}

int
httpd_get_connection_socket (httpd_t *httpd, void *user_data) {
    for (int i = 0; i < httpd->max_connections; i++) {
        http_connection_t *connection = &httpd->connections[i];
	if (!connection->connected) {
            continue;
        }
        if (connection->user_data == user_data) {
            return connection->socket_fd;
        }
    }
    return -1;
}

int
httpd_set_connection_type (httpd_t *httpd, void *user_data, connection_type_t type) {
    for (int i = 0; i < httpd->max_connections; i++) {
        http_connection_t *connection = &httpd->connections[i];
	if (!connection->connected) {
            continue;
        }
        if (connection->user_data == user_data) {
            connection->type = type;
            return i;
        }
    }
    return -1;
}
  
int
httpd_count_connection_type (httpd_t *httpd, connection_type_t type) {
    int count = 0;
    for (int i = 0; i < httpd->max_connections; i++) {
        http_connection_t *connection = &httpd->connections[i];
        if (!connection->connected) {
            continue;
        }
        if (connection->type == type) {
            count++;
        }
    }
    return count;
}

int
httpd_get_connection_socket_by_type (httpd_t *httpd, connection_type_t type, int instance){
    int count = 0;
    for (int i = 0; i < httpd->max_connections; i++) {
        http_connection_t *connection = &httpd->connections[i];
        if (!connection->connected) {
            continue;
        }
        if (connection->type == type) {
            count++;
            if (count == instance) {
                return connection->socket_fd;
            }
        }
    }
    return 0;
}

void *
httpd_get_connection_by_type (httpd_t *httpd, connection_type_t type, int instance){
    int count = 0;
    for (int i = 0; i < httpd->max_connections; i++) {
        http_connection_t *connection = &httpd->connections[i];
        if (!connection->connected) {
            continue;
        }
        if (connection->type == type) {
            count++;
            if (count == instance) {
                return connection->user_data;
            }
        }
    }
    return NULL;
}

#define MAX_CONNECTIONS 12  /* value used in AppleTV 3*/
httpd_t *
httpd_init(logger_t *logger, httpd_callbacks_t *callbacks, int nohold)
{
    httpd_t *httpd;
    assert(logger);
    assert(callbacks);

    /* Allocate the httpd_t structure */
    httpd = calloc(1, sizeof(httpd_t));
    if (!httpd) {
        return NULL;
    }

    httpd->nohold = (nohold ? 1 : 0);
    httpd->max_connections = MAX_CONNECTIONS;  
    httpd->connections = calloc(httpd->max_connections, sizeof(http_connection_t));
    if (!httpd->connections) {
        free(httpd);
        return NULL;
    }

    /* Use the logger provided */
    httpd->logger = logger;

    /* Save callback pointers */
    memcpy(&httpd->callbacks, callbacks, sizeof(httpd_callbacks_t));

    /* Initial status joined */
    httpd->running = 0;
    httpd->joined = 1;

    return httpd;
}

void
httpd_destroy(httpd_t *httpd)
{
    if (httpd) {
        httpd_stop(httpd);

        free(httpd->connections);
        free(httpd);
    }
}

static void
httpd_remove_connection(httpd_t *httpd, http_connection_t *connection)
{
    int socket_fd = connection->socket_fd;
    connection->socket_fd = 0;
    if (connection->request) {
        http_request_destroy(connection->request);
        connection->request = NULL;
    }
    logger_log(httpd->logger, LOGGER_DEBUG, "removing connection type %s socket %d conn %p", typename[connection->type],
               socket_fd, connection->user_data);
    if (connection->user_data) {
        httpd->callbacks.conn_destroy(connection->user_data);
        connection->user_data = NULL;
    }
    if (socket_fd) {
        shutdown(socket_fd, SHUT_WR);
        int ret = closesocket(socket_fd);
        if (ret == -1) {
            logger_log(httpd->logger, LOGGER_ERR, "httpd error in closesocket (close): %d %s", errno, strerror(errno));
        } else {
            logger_log(httpd->logger, LOGGER_INFO, "Connection closed on socket %d", socket_fd);
        }
    }
    if (connection->connected) {
        connection->connected = 0;
        httpd->open_connections--;
    }
    connection->type = CONNECTION_TYPE_UNKNOWN;
}

static int
httpd_add_connection(httpd_t *httpd, int fd, unsigned char *local, int local_len, unsigned char *remote,
                     int remote_len, unsigned int zone_id)
{
    void *user_data;
    int i;

    for (i=0; i<httpd->max_connections; i++) {
        if (!httpd->connections[i].connected) {
            break;
        }
    }
    if (i == httpd->max_connections) {
        /* This code should never be reached, we do not select server_fds when full */
        logger_log(httpd->logger, LOGGER_INFO, "Max connections reached");
        return -1;
    }
    user_data = httpd->callbacks.conn_init(httpd->callbacks.opaque, local, local_len, remote, remote_len, zone_id);
    if (!user_data) {
        logger_log(httpd->logger, LOGGER_ERR, "Error initializing HTTP request handler");
        return -1;
    }

    httpd->open_connections++;
    httpd->connections[i].socket_fd = fd;
    httpd->connections[i].connected = 1;
    httpd->connections[i].user_data = user_data;
    httpd->connections[i].type = CONNECTION_TYPE_UNKNOWN;   //should not be necessary ...
    return 0;
}

static int
httpd_accept_connection(httpd_t *httpd, int server_fd, int is_ipv6)
{
    struct sockaddr_storage remote_saddr;
    socklen_t remote_saddrlen;
    struct sockaddr_storage local_saddr;
    socklen_t local_saddrlen;
    unsigned char *local, *remote;
    unsigned short port;
    unsigned int local_zone_id, remote_zone_id;
    int local_len, remote_len;
    int ret, fd;

    remote_saddrlen = sizeof(remote_saddr);
    fd = accept(server_fd, (struct sockaddr *)&remote_saddr, &remote_saddrlen);
    if (fd == -1) {
        /* FIXME: Error happened */
        return -1;
    }

    local_saddrlen = sizeof(local_saddr);
    ret = getsockname(fd, (struct sockaddr *)&local_saddr, &local_saddrlen);
    if (ret == -1) {
        shutdown(fd, SHUT_RDWR);
        closesocket(fd);
        return 0;
    }

    local = netutils_get_address(&local_saddr, &local_len, &local_zone_id, &port);   
    logger_log(httpd->logger, LOGGER_INFO, "Accepted %s client on socket %d, port %u",
               (is_ipv6 ? "IPv6"  : "IPv4"), fd, port);
    remote = netutils_get_address(&remote_saddr, &remote_len, &remote_zone_id, NULL);

    // is it correct that ipv6 link-local local and remote zone id should be the same, as asserted below? 
    if (local_zone_id != remote_zone_id) {
        logger_log(httpd->logger, LOGGER_INFO, "ipv6 zone_id mismatch: local_zone_id = %u, remote_zone_id = %u",
		   local_zone_id, remote_zone_id);
    }      
    assert (local_zone_id == remote_zone_id);

    ret = httpd_add_connection(httpd, fd, local, local_len, remote, remote_len, local_zone_id);
    if (ret == -1) {
        shutdown(fd, SHUT_RDWR);
        closesocket(fd);
        return 0;
    }
    return 1;
}

bool
httpd_nohold(httpd_t *httpd) {
    return (httpd->nohold ? true: false);
}

void
httpd_remove_known_connections(httpd_t *httpd) {
    for (int i = 0; i < httpd->max_connections; i++) {
        http_connection_t *connection = &httpd->connections[i];
        if (!connection->connected || connection->type == CONNECTION_TYPE_UNKNOWN) {
            continue;
        }
        httpd_remove_connection(httpd, connection);
    }
}

void
httpd_remove_connections_by_type(httpd_t *httpd, connection_type_t type) {
    for (int i = 0; i < httpd->max_connections; i++) {
        http_connection_t *connection = &httpd->connections[i];
        if (!connection->connected || connection->type != type) {
            continue;
        }
        httpd_remove_connection(httpd, connection);
    }
}

static THREAD_RETVAL
httpd_thread(void *arg)
{
    httpd_t *httpd = arg;
    char http[] = "HTTP/1.1";
    char event[] = "EVENT/1.0";
    char buffer[1024];
    int i;

    bool logger_debug = (logger_get_level(httpd->logger) >= LOGGER_DEBUG);
    assert(httpd);

    while (1) {
        fd_set rfds;
        struct timeval tv;
        int nfds=0;
        int ret;
        int new_request;

        MUTEX_LOCK(httpd->run_mutex);
        if (!httpd->running) {
            MUTEX_UNLOCK(httpd->run_mutex);
            break;
        }
        MUTEX_UNLOCK(httpd->run_mutex);

        /* Set timeout value to 5ms */
        tv.tv_sec = 1;
        tv.tv_usec = 5000;

        /* Get the correct nfds value and set rfds */
        FD_ZERO(&rfds);
        if (httpd->open_connections < httpd->max_connections) {
            if (httpd->server_fd4 != -1) {
                FD_SET(httpd->server_fd4, &rfds);
                if (nfds <= httpd->server_fd4) {
                    nfds = httpd->server_fd4+1;
                }
            }
            if (httpd->server_fd6 != -1) {
                FD_SET(httpd->server_fd6, &rfds);
                if (nfds <= httpd->server_fd6) {
                    nfds = httpd->server_fd6+1;
                }
            }
        }
        for (i=0; i<httpd->max_connections; i++) {
            int socket_fd;
            if (!httpd->connections[i].connected) {
                continue;
            }
            socket_fd = httpd->connections[i].socket_fd;
            FD_SET(socket_fd, &rfds);
            if (nfds <= socket_fd) {
                nfds = socket_fd+1;
            }
        }

        ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret == 0) {
            /* Timeout happened */
            continue;
        } else if (ret == -1) {
            int sock_err = SOCKET_GET_ERROR();
            logger_log(httpd->logger, LOGGER_ERR,
		       "httpd error in select: %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
            break;
        }

        if (httpd->open_connections < httpd->max_connections &&
            httpd->server_fd4 != -1 && FD_ISSET(httpd->server_fd4, &rfds)) {
            ret = httpd_accept_connection(httpd, httpd->server_fd4, 0);
            if (ret == -1) {
                logger_log(httpd->logger, LOGGER_ERR, "httpd error in accept ipv4");
                break;
            } else if (ret == 0) {
                continue;
            }
        }
        if (httpd->open_connections < httpd->max_connections &&
            httpd->server_fd6 != -1 && FD_ISSET(httpd->server_fd6, &rfds)) {
            ret = httpd_accept_connection(httpd, httpd->server_fd6, 1);
            if (ret == -1) {
                logger_log(httpd->logger, LOGGER_ERR, "httpd error in accept ipv6");
                break;
            } else if (ret == 0) {
                continue;
            }
        }
        for (i=0; i<httpd->max_connections; i++) {
            http_connection_t *connection = &httpd->connections[i];

            if (!connection->connected) {
                continue;
            }
            if (!FD_ISSET(connection->socket_fd, &rfds)) {
                continue;
            }

            /* If not in the middle of request, allocate one */
            if (!connection->request) {
                connection->request = http_request_init();
                assert(connection->request);
                new_request = 1;
                if (connection->type == CONNECTION_TYPE_PTTH) {
                    http_request_is_reverse(connection->request);
                }
                logger_log(httpd->logger, LOGGER_DEBUG, "new request, connection %d, socket %d type %s",
                           i, connection->socket_fd, typename [connection->type]);
            } else {
                new_request = 0;
            }

            logger_log(httpd->logger, LOGGER_DEBUG, "httpd receiving on socket %d, connection %d",
                       connection->socket_fd, i);
            if (logger_debug) {
                logger_log(httpd->logger, LOGGER_DEBUG,"\nhttpd: current connections:");
                for (int i = 0; i < httpd->max_connections; i++) {
                    http_connection_t *connection = &httpd->connections[i];
                    if (!connection->connected || !connection->socket_fd) {
                        continue;
                    }
                    if (!FD_ISSET(connection->socket_fd, &rfds)) {
                        logger_log(httpd->logger, LOGGER_DEBUG, "connection %d type %d socket %d  conn %p %s", i,
                                   connection->type, connection->socket_fd,
                                   connection->user_data, typename [connection->type]);
                    } else {
                        logger_log(httpd->logger, LOGGER_DEBUG, "connection %d type %d socket %d  conn %p %s ACTIVE CONNECTION",
                                   i, connection->type, connection->socket_fd, connection->user_data, typename [connection->type]);
                    }
                }
                logger_log(httpd->logger, LOGGER_DEBUG, " ");
            }
            /* reverse-http responses from the client must not be sent to the llhttp parser:
             * such messages start with "HTTP/1.1" (or sometimes with "EVENT/1.0")  */
            if (new_request) {
                int readstart = 0;
                new_request = 0;
                while (readstart < 8) {
                    if (!connection->socket_fd) {
                        break;
                    }
                    ret = recv(connection->socket_fd, buffer + readstart, sizeof(buffer) - 1 - readstart, 0);
                    if (ret == 0) {
                        logger_log(httpd->logger, LOGGER_DEBUG, "client closed connection on socket %d",
                                   connection->socket_fd);
                        break;
                    } else if (ret == -1) {
                        if (errno == EAGAIN) {
                            continue;
                        } else {
                            int sock_err = SOCKET_GET_ERROR();
                            logger_log(httpd->logger, LOGGER_ERR, "httpd: recv error %d on socket %d: %s",
                                       sock_err, connection->socket_fd, SOCKET_ERROR_STRING(sock_err));
                            break;
                        }
                    } else {
                        readstart += ret;
                        ret = readstart;
                    }
                }
                if (!connection->socket_fd) {
                    /* connection was recently removed */
                    continue;
                }
                if (!memcmp(buffer, http, 8) || !memcmp(buffer, event, 8)) {
                    http_request_set_reverse(connection->request);  
                }
            } else {
                if (connection->socket_fd) {
                    ret = recv(connection->socket_fd, buffer, sizeof(buffer) - 1, 0);
                    if (ret == 0) {
                        httpd_remove_connection(httpd, connection);
                        continue;
                    }
                } else {
                    /* connection was recently removed */
                    continue;
                }
            }
            if (http_request_is_reverse(connection->request)) {
                /* this is a response from the client to a
                 * GET /event reverse HTTP request from the server */
                if (ret && logger_debug) {
                    buffer[ret] = '\0';
                    logger_log(httpd->logger, LOGGER_INFO, "<<<< received response from client"
                               " (reversed HTTP = \"PTTH/1.0\") connection"
                               " on socket %d:\n%s\n", connection->socket_fd, buffer);
                }
                if (ret == 0) {
                    httpd_remove_connection(httpd, connection);
                }
                continue;
            }

            /* Parse HTTP request from data read from connection */
            http_request_add_data(connection->request, buffer, ret);
            if (http_request_has_error(connection->request)) {
                char *data = utils_data_to_text((const char *) buffer, ret);
                logger_log(httpd->logger, LOGGER_ERR, "httpd error in parsing: %s\n%s\n%s",
                           http_request_get_error_name(connection->request),
                           http_request_get_error_description(connection->request),
                           data);
                free (data);
                httpd_remove_connection(httpd, connection);
                continue;
            }

            /* If request is finished, process and deallocate */
            if (http_request_is_complete(connection->request)) {
                http_response_t *response = NULL;
                // Callback the received data to raop
                if (logger_debug) {
                    const char *method = http_request_get_method(connection->request);
                    const char *url = http_request_get_url(connection->request);
                    const char *protocol = http_request_get_protocol(connection->request);
                    logger_log(httpd->logger, LOGGER_INFO, "httpd request received on socket %d, "
                               "connection %d, method = %s, url = %s, protocol = %s",
                               connection->socket_fd, i, method, url, protocol);
                }
                httpd->callbacks.conn_request(connection->user_data, connection->request, &response);
                http_request_destroy(connection->request);
                connection->request = NULL;

                if (response) {
                    const char *data;
                    int datalen;
                    int written;
                    int ret;

                    /* Get response data and datalen */
                    data = http_response_get_data(response, &datalen);

                    written = 0;
                    while (written < datalen) {
                        ret = send(connection->socket_fd, data+written, datalen-written, 0);
                        if (ret == -1) {
                            logger_log(httpd->logger, LOGGER_ERR, "httpd error in sending data");
                            break;
                        }
                        written += ret;
                    }

                    if (http_response_get_disconnect(response)) {
                        logger_log(httpd->logger, LOGGER_INFO, "Disconnecting on software request");
                        httpd_remove_connection(httpd, connection);
                    }
                } else {
                    logger_log(httpd->logger, LOGGER_WARNING, "httpd didn't get response");
                }
                http_response_destroy(response);
            } else {
                logger_log(httpd->logger, LOGGER_DEBUG, "Request not complete, waiting for more data...");
            }
        }
    }

    /* Remove all connections that are still connected */
    for (i=0; i<httpd->max_connections; i++) {
        http_connection_t *connection = &httpd->connections[i];

        if (!connection->connected) {
            continue;
        }
        logger_log(httpd->logger, LOGGER_INFO, "Removing connection for socket %d", connection->socket_fd);
        httpd_remove_connection(httpd, connection);
    }

    /* Close server sockets since they are not used any more */
    if (httpd->server_fd4 != -1) {
        shutdown(httpd->server_fd4, SHUT_RDWR);
        closesocket(httpd->server_fd4);
        httpd->server_fd4 = -1;
    }
    if (httpd->server_fd6 != -1) {
        shutdown(httpd->server_fd6, SHUT_RDWR);
        closesocket(httpd->server_fd6);
        httpd->server_fd6 = -1;
    }

    // Ensure running reflects the actual state
    MUTEX_LOCK(httpd->run_mutex);
    httpd->running = 0;
    MUTEX_UNLOCK(httpd->run_mutex);

    logger_log(httpd->logger, LOGGER_DEBUG, "Exiting httpd thread");

    return 0;
}

int
httpd_start(httpd_t *httpd, unsigned short *port)
{
    /* How many connection attempts are kept in queue */
    int backlog = 5;

    assert(httpd);
    assert(port);

    MUTEX_LOCK(httpd->run_mutex);
    if (httpd->running || !httpd->joined) {
        MUTEX_UNLOCK(httpd->run_mutex);
        return 0;
    }

    httpd->server_fd4 = netutils_init_socket(port, 0, 0);
    if (httpd->server_fd4 == -1) {
        logger_log(httpd->logger, LOGGER_ERR, "Error initialising socket %d", SOCKET_GET_ERROR());
        MUTEX_UNLOCK(httpd->run_mutex);
        return -1;
    }
    httpd->server_fd6 = netutils_init_socket(port, 1, 0);
        if (httpd->server_fd6 == -1) {
            logger_log(httpd->logger, LOGGER_WARNING, "Error initialising IPv6 socket %d", SOCKET_GET_ERROR());
            logger_log(httpd->logger, LOGGER_WARNING, "Continuing without IPv6 support");
        }

    if (httpd->server_fd4 != -1 && listen(httpd->server_fd4, backlog) == -1) {
        logger_log(httpd->logger, LOGGER_ERR, "Error listening to IPv4 socket");
        closesocket(httpd->server_fd4);
        closesocket(httpd->server_fd6);
        MUTEX_UNLOCK(httpd->run_mutex);
        return -2;
    }
    if (httpd->server_fd6 != -1 && listen(httpd->server_fd6, backlog) == -1) {
        logger_log(httpd->logger, LOGGER_ERR, "Error listening to IPv6 socket");
        closesocket(httpd->server_fd4);
        closesocket(httpd->server_fd6);
        MUTEX_UNLOCK(httpd->run_mutex);
        return -2;
    }
    logger_log(httpd->logger, LOGGER_INFO, "Initialized server socket(s)");

    /* Set values correctly and create new thread */
    httpd->running = 1;
    httpd->joined = 0;
    THREAD_CREATE(httpd->thread, httpd_thread, httpd);
    MUTEX_UNLOCK(httpd->run_mutex);

    return 1;
}

int
httpd_is_running(httpd_t *httpd)
{
    int running;

    assert(httpd);

    MUTEX_LOCK(httpd->run_mutex);
    running = httpd->running || !httpd->joined;
    MUTEX_UNLOCK(httpd->run_mutex);

    return running;
}

void
httpd_stop(httpd_t *httpd)
{
    assert(httpd);

    MUTEX_LOCK(httpd->run_mutex);
    if (!httpd->running || httpd->joined) {
        MUTEX_UNLOCK(httpd->run_mutex);
        return;
    }
    httpd->running = 0;
    MUTEX_UNLOCK(httpd->run_mutex);

    THREAD_JOIN(httpd->thread);

    MUTEX_LOCK(httpd->run_mutex);
    httpd->joined = 1;
    MUTEX_UNLOCK(httpd->run_mutex);
}
