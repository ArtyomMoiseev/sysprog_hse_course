#include "chat.h"
#include "chat_server.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/event.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <arpa/inet.h>

struct message {
    char *data;
    struct message *next;
};

struct chat_peer {
	/** Client's socket. To read/write messages. */
	int socket;
	/** Output buffer. */
	char out_buff[1024];
    size_t out_buff_size;

    char recv_buff[1024];
    size_t recv_buff_size;
};

struct chat_server {
	/** Listening socket. To accept new clients. */
	int socket;
    int kq;
	/** Array of peers. */
	struct chat_peer *peers;
    size_t peers_count;

    struct message *messages_head;
    struct message *messages_tail;

    int is_listen;
};

struct chat_server *
chat_server_new(void)
{
	struct chat_server *server = calloc(1, sizeof(*server));

    server->socket = -1;
    server->kq = kqueue();
    return server;
}

void
chat_server_delete(struct chat_server *server)
{
	if (server->socket >= 0)
		close(server->socket);

    if (server->socket >= 0) {
        close(server->socket);
        server->socket = -1;
    }

    if (server->kq >= 0) {
        close(server->kq);
        server->kq = -1;
    }

    for (size_t i = 0; i < server->peers_count; i++) {
        struct chat_peer *p = &server->peers[i];
        close(p->socket);
    }
    free(server->peers);

    struct message *m = server->messages_head;
    while (m) {
        struct message *tmp = m->next;
        free(m->data);
        free(m);
        m = tmp;
    }

	free(server);
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_port = htons(port);
	/* Listen on all IPs of this machine. */
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (!server) {
        return CHAT_ERR_SYS;
    }
    if (server->is_listen) {
        return CHAT_ERR_ALREADY_STARTED;
    }

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        return CHAT_ERR_SYS;
    }

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opt = 1);
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {

        close(srv_fd);
        return CHAT_ERR_SYS;
    }

    if (listen(srv_fd, 128) < 0) {
        close(srv_fd);
        return CHAT_ERR_SYS;
    }

    server->socket = srv_fd;
    server->is_listen = 1;

    struct kevent evset;
    EV_SET(&evset, srv_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(server->kq, &evset, 1, NULL, 0, NULL) < 0) {
        close(srv_fd);
        server->socket = -1;
        server->is_listen = 0;
        return CHAT_ERR_SYS;
    }

    return 0;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);


    if (listen(srv_fd, 128) < 0) {
        close(srv_fd);
        return CHAT_ERR_SYS;
    }

    server->socket = srv_fd;
    server->is_listen = 1;

    EV_SET(&evset, srv_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(server->kq, &evset, 1, NULL, 0, NULL) < 0) {
        close(srv_fd);
        server->socket = -1;
        server->is_listen = 0;
        return CHAT_ERR_SYS;
    }

    return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)server;
	return NULL;
}

int
chat_server_update(struct chat_server *server, double timeout)
{
	/*
	 * 1) Wait on epoll/kqueue/poll for update on any socket.
	 * 2) Handle the update.
	 * 2.1) If the update was on listen-socket, then you probably need to
	 *     call accept() on it - a new client wants to join.
	 * 2.2) If the update was on a client-socket, then you might want to
	 *     read/write on it.
	 */
        if (!server)
          return CHAT_ERR_SYS;

    struct kevent events[64];
    struct timespec ts;
    if (timeout >= 0) {
        ts.tv_sec  = (time_t)timeout;
        ts.tv_nsec = (long)((timeout - ts.tv_sec) * 1e9);
    }

    int nev = kevent(server->kq, NULL, 0, events, 64,
                     (timeout >= 0 ? &ts : NULL));

    for (int i = 0; i < nev; i++) {
        struct kevent *kev = &events[i];

        if (kev->ident == (uintptr_t)server->socket) {
            if (kev->filter == EVFILT_READ) {
                printf("chat_server_event socker=%d\n", server->socket);
            }
        }
        else {
          printf("chat_server_event socker=%d\n", server->socket);
        }
    }

    return 0;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */

	/*
	 * Server has multiple sockets - own and from connected clients. Hence
	 * you can't return a socket here. But if you are using epoll/kqueue,
	 * then you can return their descriptor. These descriptors can be polled
	 * just like sockets and will return an event when any of their owned
	 * descriptors has any events.
	 *
	 * For example, assume you created an epoll descriptor and added to
	 * there a listen-socket and a few client-sockets. Now if you will call
	 * poll() on the epoll's descriptor, then on return from poll() you can
	 * be sure epoll_wait() can return something useful for some of those
	 * sockets.
	 */
#endif
	(void)server;
	return -1;
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
	/*
	 * IMPLEMENT THIS FUNCTION - add OUTPUT event if has non-empty output
	 * buffer in any of the client-sockets.
	 */
	(void)server;
	return CHAT_EVENT_INPUT;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */
#endif
	(void)server;
	(void)msg;
	(void)msg_size;
	return CHAT_ERR_NOT_IMPLEMENTED;
}