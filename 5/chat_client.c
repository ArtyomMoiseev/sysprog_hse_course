#include "chat.h"
#include "chat_client.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/event.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>

struct message {
    char *data;
    struct message *next;
};

struct chat_client {
	/** Socket connected to the server. */
	int socket;
	/** Array of received messages. */
    struct message *messages_recv_head;
    struct message *messages_recv_tail;
	/** Output buffer. */
	char  out_buff[1024];
    size_t out_buff_size;

    int kq;
};

struct chat_client *
chat_client_new(const char *name)
{
    (void)name;

    struct chat_client *client = calloc(1, sizeof(*client));
    client->socket = -1;
    client->kq = kqueue();
    return client;
}

void
chat_client_delete(struct chat_client *client)
{
	if (!client)
          return;

    if (client->socket >= 0) {
        close(client->socket);
        client->socket = -1;
    }

    if (client->kq >= 0) {
        close(client->kq);
        client->kq = -1;
    }

    struct message *m = client->messages_recv_head;

    while (m) {
        struct message *tmp = m->next;
        free(m->data);
        free(m);
        m = tmp;
    }

    free(client);
}

int
chat_client_connect(struct chat_client *client, const char *addr)
{
	/*
	 * 1) Use getaddrinfo() to resolve addr to struct sockaddr_in.
	 * 2) Create a client socket (function socket()).
	 * 3) Connect it by the found address (function connect()).
	 */
	if (!client || !addr)
          return CHAT_ERR_SYS;

    const char *colon = strrchr(addr, ':');
    char host[256];
    size_t host_len = (size_t)(colon - addr);
    if (host_len >= sizeof(host)) {
        fprintf(stderr, "Host name too long.\n");
        return CHAT_ERR_SYS;
    }
    memcpy(host, addr, host_len);
    host[host_len] = '\0';

    char port[32];
    strncpy(port, colon + 1, sizeof(port) - 1);
    port[sizeof(port) - 1] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;

    int rv = getaddrinfo(host, port, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo('%s','%s') failed: %s\n",
                host, port, gai_strerror(rv));
        return CHAT_ERR_SYS;
    }

    int sock = -1;
    struct addrinfo *rp;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);

    if (sock < 0) {
        fprintf(stderr, "Unable to connect");
        return CHAT_ERR_TIMEOUT;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags != -1) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    client->socket = sock;

    struct kevent evset[2];
    EV_SET(&evset[0], sock, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    EV_SET(&evset[1], sock, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(client->kq, evset, 2, NULL, 0, NULL) < 0) {
        fprintf(stderr, "kevent registration failed.\n");
        close(sock);
        client->socket = -1;
        return CHAT_ERR_SYS;
    }
    return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)client;
	return NULL;
}

int
chat_client_update(struct chat_client *client, double timeout)
{
	/*
	 * The easiest way to wait for updates on a single socket with a timeout
	 * is to use poll(). Epoll is good for many sockets, poll is good for a
	 * few.
	 *
	 * You create one struct pollfd, fill it, call poll() on it, handle the
	 * events (do read/write).
	 */
	(void)client;
	(void)timeout;
	return CHAT_ERR_NOT_IMPLEMENTED;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int
chat_client_get_events(const struct chat_client *client)
{
	/*
	 * IMPLEMENT THIS FUNCTION - add OUTPUT event if has non-empty output
	 * buffer.
	 */
	(void)client;
	return CHAT_EVENT_INPUT;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)client;
	(void)msg;
	(void)msg_size;
	return CHAT_ERR_NOT_IMPLEMENTED;
}
