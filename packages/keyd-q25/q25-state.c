/* SPDX-License-Identifier: MIT */
#include "keyd.h"
#include "q25-state.h"

#define MAX_STATE_CLIENTS 8
static int state_fd = -1;
static int clients[MAX_STATE_CLIENTS];
static size_t nclients;
static uint8_t snapshot[4] = {'Q', 1, 0, 0};

/* Separate from keyd's privileged command socket. Only the daemon can publish.
 * All IO is nonblocking, queues and client count are bounded; slow clients lose
 * the connection rather than holding up keyboard input. Reconnect resyncs. */
int q25_state_init(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (strlen(path) >= sizeof(addr.sun_path))
		return -1;
	strcpy(addr.sun_path, path);
	state_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (state_fd < 0)
		return -1;
	/* The caller holds keyd's singleton lock; parent directory is root-owned. */
	unlink(path);
	if (bind(state_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    chmod(path, 0666) < 0 || listen(state_fd, MAX_STATE_CLIENTS) < 0) {
		close(state_fd);
		state_fd = -1;
		return -1;
	}
	return state_fd;
}

static int client_ready(int fd)
{
	struct pollfd p = {.fd = fd, .events = POLLIN | POLLOUT};
	/* Input (including EOF) is never a request: this is a receive-only API. */
	return poll(&p, 1, 0) == 1 && p.revents == POLLOUT;
}

static void prune_clients(void)
{
	size_t n = 0;
	for (size_t i = 0; i < nclients; i++) {
		if (client_ready(clients[i]))
			clients[n++] = clients[i];
		else
			close(clients[i]);
	}
	nclients = n;
}

void q25_state_accept(void)
{
	/* At most one accept per event-loop iteration, even under a connection flood. */
	int con = accept(state_fd, NULL, NULL);
	int size = 1024;
	if (con < 0)
		return;
	if (fcntl(con, F_SETFL, O_NONBLOCK) < 0 ||
	    fcntl(con, F_SETFD, FD_CLOEXEC) < 0) {
		close(con);
		return;
	}
	setsockopt(con, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
	prune_clients();
	if (nclients == MAX_STATE_CLIENTS ||
	    send(con, snapshot, sizeof(snapshot), MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof(snapshot)) {
		close(con);
		return;
	}
	clients[nclients++] = con;
}

void q25_state_publish(uint8_t shift, uint8_t sym)
{
	size_t n = 0;
	prune_clients();
	if (snapshot[2] == shift && snapshot[3] == sym)
		return;
	snapshot[2] = shift;
	snapshot[3] = sym;
	for (size_t i = 0; i < nclients; i++) {
		if (send(clients[i], snapshot, sizeof(snapshot), MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof(snapshot))
			clients[n++] = clients[i];
		else
			close(clients[i]);
	}
	nclients = n;
}

int q25_state_layers(const struct keyboard *kbd, uint8_t *shift, uint8_t *sym)
{
	uint8_t flags[2] = {0};
	int q25 = 0;
	for (size_t i = 1; i < kbd->config.nr_layers; i++) {
		int target;
		if (!strcmp(kbd->config.layers[i].name, "shift"))
			target = 0;
		else if (!strcmp(kbd->config.layers[i].name, "ralt_sym_layer2")) {
			target = 1;
			q25 = 1;
		} else
			continue;
		unsigned active = kbd->layer_state[i].active;
		unsigned locked = kbd->layer_state[i].toggled;
		unsigned oneshot = kbd->layer_state[i].oneshot_depth;
		flags[target] = (active > locked + oneshot ? 1 : 0) |
			(oneshot ? 2 : 0) | (locked ? 4 : 0);
	}
	if (q25) {
		*shift |= flags[0];
		*sym |= flags[1];
	}
	return q25;
}
