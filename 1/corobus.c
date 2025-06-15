#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct data_vector {
	unsigned *data;
	size_t size;
	size_t capacity;
};

static void data_vector_clear(struct data_vector *vec) {
    free(vec->data);
    vec->data = NULL;
    vec->size = vec->capacity = 0;
}

static void data_vector_append(struct data_vector *vec, unsigned val) {
    if (vec->size == vec->capacity) {
        vec->capacity = vec->capacity ? vec->capacity * 2 : 4;
        vec->data = realloc(vec->data, vec->capacity * sizeof(unsigned));
    }
    vec->data[vec->size++] = val;
}

static unsigned data_vector_pop_first(struct data_vector *vec) {
    assert(vec->size > 0);
    unsigned val = vec->data[0];
    memmove(vec->data, vec->data + 1, (--vec->size) * sizeof(unsigned));
    return val;
}

#if 0 /* Uncomment this if want to use */

/** Append @a count messages in @a data to the end of the vector. */
static void
data_vector_append_many(struct data_vector *vector,
	const unsigned *data, size_t count)
{
	if (vector->size + count > vector->capacity) {
		if (vector->capacity == 0)
			vector->capacity = 4;
		else
			vector->capacity *= 2;
		if (vector->capacity < vector->size + count)
			vector->capacity = vector->size + count;
		vector->data = realloc(vector->data,
			sizeof(vector->data[0]) * vector->capacity);
	}
	memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
	vector->size += count;
}

/** Append a single message to the vector. */
static void
data_vector_append(struct data_vector *vector, unsigned data)
{
	data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void
data_vector_pop_first_many(struct data_vector *vector, unsigned *data, size_t count)
{
	assert(count <= vector->size);
	memcpy(data, vector->data, sizeof(data[0]) * count);
	vector->size -= count;
	memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
}

/** Pop a single message from the head of the vector. */
static unsigned
data_vector_pop_first(struct data_vector *vector)
{
	unsigned data = 0;
	data_vector_pop_first_many(vector, &data, 1);
	return data;
}

#endif

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
	struct rlist coros;
};

static void wakeup_queue_init(struct wakeup_queue *queue) {
    rlist_create(&queue->coros);
}

static void wakeup_first(struct wakeup_queue *queue) {
    if (rlist_empty(&queue->coros))
        return;
    struct wakeup_entry *entry = rlist_shift_entry(&queue->coros, struct wakeup_entry, base);
    coro_wakeup(entry->coro);
}

#if 0 /* Uncomment this if want to use */

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	coro_wakeup(entry->coro);
}

#endif

struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	struct data_vector data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *bus = calloc(1, sizeof(*bus));
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return bus;
}

void
coro_bus_delete(struct coro_bus *bus)
{
	for (int i = 0; i < bus->channel_count; ++i) {
        if (bus->channels[i]) {
            data_vector_clear(&bus->channels[i]->data);
            free(bus->channels[i]);
        }
    }
    free(bus->channels);
    free(bus);
}

static struct coro_bus_channel *get_channel(struct coro_bus *bus, int ch_id) {
    return (ch_id >= 0 && ch_id < bus->channel_count) ? bus->channels[ch_id] : NULL;
}

int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
	struct coro_bus_channel *ch = calloc(1, sizeof(*ch));
    ch->size_limit = size_limit;
    wakeup_queue_init(&ch->send_queue);
    wakeup_queue_init(&ch->recv_queue);

    for (int i = 0; i < bus->channel_count; ++i)
        if (!bus->channels[i]) {
            bus->channels[i] = ch;
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            return i;
        }

    bus->channels = realloc(bus->channels, ++bus->channel_count * sizeof(*bus->channels));
    bus->channels[bus->channel_count - 1] = ch;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return bus->channel_count - 1;
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel)
{
	struct coro_bus_channel *ch = get_channel(bus, channel);
    if (!ch) return;

    data_vector_clear(&ch->data);

    struct wakeup_entry *entry, *tmp;
    rlist_foreach_entry_safe(entry, &ch->send_queue.coros, base, tmp) {
        coro_wakeup(entry->coro);
        rlist_del_entry(entry, base);
    }
    rlist_foreach_entry_safe(entry, &ch->recv_queue.coros, base, tmp) {
        coro_wakeup(entry->coro);
        rlist_del_entry(entry, base);
    }
    free(ch);
    bus->channels[channel] = NULL;
}

int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
	struct coro_bus_channel *ch = get_channel(bus, channel);
    if (!ch) return coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL), -1;

    if (ch->data.size >= ch->size_limit)
        return coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK), -1;

    data_vector_append(&ch->data, data);
    wakeup_first(&ch->recv_queue);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
	while (coro_bus_try_send(bus, channel, data) != 0) {
        if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
            return -1;

        struct wakeup_entry entry = {.coro = coro_this()};
        rlist_add_tail_entry(&get_channel(bus, channel)->send_queue.coros, &entry, base);

        coro_suspend();

        if (rlist_entry_is_head(&entry, &get_channel(bus, channel)->send_queue.coros, base))
            continue;
        rlist_del_entry(&entry, base);
    }
    return 0;
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	struct coro_bus_channel *ch = get_channel(bus, channel);
    if (!ch) return coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL), -1;

    if (ch->data.size == 0)
        return coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK), -1;

    *data = data_vector_pop_first(&ch->data);
    wakeup_first(&ch->send_queue);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	while (coro_bus_try_recv(bus, channel, data) != 0) {
        if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
            return -1;

        struct wakeup_entry entry = {.coro = coro_this()};
        rlist_add_tail_entry(&get_channel(bus, channel)->recv_queue.coros, &entry, base);

        coro_suspend();

        if (rlist_entry_is_head(&entry, &get_channel(bus, channel)->recv_queue.coros, base))
            continue;
        rlist_del_entry(&entry, base);
    }
    return 0;
}


#if NEED_BROADCAST

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)data;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)data;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)count;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)count;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)capacity;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)capacity;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

#endif