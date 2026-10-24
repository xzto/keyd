#include "keyd.h"

#include <sys/inotify.h>

struct config_ent {
	struct config config;
	struct keyboard *kbd;
	struct config_ent *next;
};

/*
 * Config files are watched by directory rather than by file so that both
 * atomic saves (write + rename) and symlinked targets are handled: the
 * containing directory reports the rename no matter which inode lands at the
 * config path. Each watched directory maps to an inotify wd so events can be
 * filtered (CONFIG_DIR may hold unrelated files).
 */
struct config_watch {
	int wd;
	int is_config_dir;
	int seen;
	char path[PATH_MAX];
};

#define CONFIGWATCH_DEBOUNCE_MS 100

static int configfd = -1;
static struct config_watch config_watches[64];
static size_t nr_config_watches;
static long config_reload_deadline;

static int ipcfd = -1;
static struct vkbd *vkbd = NULL;
static struct config_ent *configs;

static uint8_t keystate[256];

static int listeners[32];
static size_t nr_listeners = 0;
static struct keyboard *active_kbd = NULL;

static long configwatch_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct config_watch *configwatch_find(int wd)
{
	size_t i;

	for (i = 0; i < nr_config_watches; i++)
		if (config_watches[i].wd == wd)
			return &config_watches[i];

	return NULL;
}

static void configwatch_add(const char *path, int is_config_dir)
{
	size_t i;
	int wd;

	for (i = 0; i < nr_config_watches; i++) {
		if (!strcmp(config_watches[i].path, path)) {
			config_watches[i].seen = 1;
			config_watches[i].is_config_dir |= is_config_dir;
			return;
		}
	}

	if (nr_config_watches == ARRAY_SIZE(config_watches)) {
		keyd_log("CONFIG: y{WARNING} too many directories to watch\n");
		return;
	}

	wd = inotify_add_watch(configfd, path, IN_CREATE | IN_DELETE |
			       IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM |
			       IN_ATTRIB | IN_MODIFY | IN_DELETE_SELF |
			       IN_MOVE_SELF);
	if (wd < 0)
		return;

	config_watches[nr_config_watches].wd = wd;
	config_watches[nr_config_watches].is_config_dir = is_config_dir;
	config_watches[nr_config_watches].seen = 1;
	snprintf(config_watches[nr_config_watches].path,
		 sizeof(config_watches[nr_config_watches].path), "%s", path);
	nr_config_watches++;
}

/*
 * Refresh the watch set after configs are (re)loaded. Symlinked configs are
 * resolved so edits to the target are noticed even when the link itself is
 * untouched. The directory is scanned directly (rather than iterating parsed
 * configs) so a file that temporarily fails to parse keeps its watch.
 */
static void configwatch_refresh(void)
{
	size_t i;
	DIR *dh;
	struct dirent *dirent;

	if (configfd < 0)
		return;

	for (i = 0; i < nr_config_watches; i++)
		config_watches[i].seen = 0;

	configwatch_add(CONFIG_DIR, 1);

	if ((dh = opendir(CONFIG_DIR))) {
		while ((dirent = readdir(dh))) {
			char path[PATH_MAX];
			char real[PATH_MAX];
			char *slash;
			size_t n = strlen(dirent->d_name);

			if (n < 5 || strcmp(dirent->d_name + n - 5, ".conf"))
				continue;

			snprintf(path, sizeof path, "%s/%s", CONFIG_DIR, dirent->d_name);
			if (!realpath(path, real))
				continue;

			slash = strrchr(real, '/');
			if (slash == real)
				slash[1] = 0;
			else if (slash)
				*slash = 0;

			configwatch_add(real, !strcmp(real, CONFIG_DIR));
		}
		closedir(dh);
	}

	for (i = 0; i < nr_config_watches; ) {
		if (config_watches[i].seen) {
			i++;
			continue;
		}

		inotify_rm_watch(configfd, config_watches[i].wd);
		config_watches[i] = config_watches[--nr_config_watches];
	}
}

/*
 * Drain all pending events and arm a debounce timer. Editors emit a burst of
 * events for a single save; a single reload after the burst avoids repeatedly
 * ungrab/regrabing devices mid-edit.
 */
static void configwatch_handle(void)
{
	char buf[4096];
	ssize_t n;

	while ((n = read(configfd, buf, sizeof buf)) > 0) {
		char *p = buf;

		while (p < buf + n) {
			struct inotify_event *ev = (struct inotify_event *)p;
			struct config_watch *watch;

			p += sizeof(*ev) + ev->len;

			if (ev->mask & IN_IGNORED)
				continue;

			watch = configwatch_find(ev->wd);
			if (!watch)
				continue;

			if (watch->is_config_dir) {
				size_t len = ev->len ? strnlen(ev->name, ev->len) : 0;

				if (len < 5 || strcmp(ev->name + len - 5, ".conf"))
					continue;
			}

			config_reload_deadline = configwatch_now() + CONFIGWATCH_DEBOUNCE_MS;
		}
	}
}

static void free_configs(struct config_ent *ent)
{
	while (ent) {
		struct config_ent *tmp = ent;
		ent = ent->next;
		free(tmp->kbd);
		free(tmp);
	}
}

static void cleanup(void)
{
	free_configs(configs);
	free_vkbd(vkbd);
}

static void clear_vkbd(void)
{
	size_t i;

	for (i = 0; i < 256; i++)
		if (keystate[i]) {
			vkbd_send_key(vkbd, i, 0);
			keystate[i] = 0;
		}
}

static void send_key(uint8_t code, uint8_t state)
{
	keystate[code] = state;

	switch (code) {
		case KEYD_SCROLL_DOWN:
			if (state)
				vkbd_mouse_scroll(vkbd, 0, -1);
			break;
		case KEYD_SCROLL_UP:
			if (state)
				vkbd_mouse_scroll(vkbd, 0, 1);
			break;
		case KEYD_SCROLL_RIGHT:
			if (state)
				vkbd_mouse_scroll(vkbd, 1, 0);
			break;
		case KEYD_SCROLL_LEFT:
			if (state)
				vkbd_mouse_scroll(vkbd, -1, 0);
			break;
		default:
			vkbd_send_key(vkbd, code, state);
			break;
	}
}

static void send_key_macro_wrapper(void *ctx, uint8_t code, uint8_t state)
{
	send_key(code, state);
}

static void add_listener(int con)
{
	struct timeval tv;

	/*
	 * In order to avoid blocking the main event loop, allow up to 50ms for
	 * slow clients to relieve back pressure before dropping them.
	 */
	tv.tv_usec = 50000;
	tv.tv_sec = 0;

	if (nr_listeners == ARRAY_SIZE(listeners)) {
		char s[] = "Max listeners exceeded\n";
		xwrite(con, &s, sizeof s);

		close(con);
		return;
	}

	setsockopt(con, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	if (active_kbd) {
		size_t i;
		struct config *config = &active_kbd->config;
		struct layer *layout = &config->layers[0];

		for (i = 1; i < config->nr_layers; i++)
			if (active_kbd->layer_state[i].active) {
				struct layer *layer = &config->layers[i];

				if (layer->type == LT_LAYOUT) {
					layout = layer;
					break;
				}
			}

		dprintf(con, "/%s\n", layout->name);

		for (i = 1; i < config->nr_layers; i++) {
			if (active_kbd->layer_state[i].active) {
				ssize_t ret;
				struct layer *layer = &config->layers[i];

				if (layer->type != LT_LAYOUT) {
					ret = dprintf(con, "+%s\n", layer->name);
					if (ret < 0)
						goto fail;
				}
			}
		}
	}

	listeners[nr_listeners++] = con;
	return;
fail:
	close(con);
	return;
}

static void on_layer_change(const struct keyboard *kbd, const struct layer *layer, uint8_t state)
{
	size_t i;
	char buf[MAX_LAYER_NAME_LEN+2];
	ssize_t bufsz;

	int keep[ARRAY_SIZE(listeners)];
	size_t n = 0;

	if (kbd->config.layer_indicator) {
		int active_layers = 0;

		for (i = 1; i < kbd->config.nr_layers; i++)
			if (kbd->config.layers[i].type != LT_LAYOUT && kbd->layer_state[i].active) {
				active_layers = 1;
				break;
			}

		for (i = 0; i < device_table_sz; i++)
			if (device_table[i].data == kbd)
				device_set_led(&device_table[i], 1, active_layers);
	}

	if (!nr_listeners)
		return;

	if (layer->type == LT_LAYOUT)
		bufsz = snprintf(buf, sizeof(buf), "/%s\n", layer->name);
	else
		bufsz = snprintf(buf, sizeof(buf), "%c%s\n", state ? '+' : '-', layer->name);

	for (i = 0; i < nr_listeners; i++) {
		ssize_t nw = write(listeners[i], buf, bufsz);

		if (nw == bufsz)
			keep[n++] = listeners[i];
		else
			close(listeners[i]);
	}

	if (n != nr_listeners) {
		nr_listeners = n;
		memcpy(listeners, keep, n * sizeof(int));
	}
}

static int load_configs(struct config_ent **result)
{
	DIR *dh = opendir(CONFIG_DIR);
	struct config_ent *candidates = NULL;
	int failed = 0;

	if (!dh) {
		perror("opendir");
		return -1;
	}

	for (;;) {
		struct dirent *dirent;
		char path[PATH_MAX];
		int len;

		errno = 0;
		dirent = readdir(dh);
		if (!dirent) {
			if (errno) {
				perror("readdir");
				failed = 1;
			}
			break;
		}
		if (dirent->d_type == DT_DIR)
			continue;

		size_t n = strlen(dirent->d_name);
		if (n < 5 || strcmp(dirent->d_name + n - 5, ".conf"))
			continue;

		len = snprintf(path, sizeof path, "%s/%s", CONFIG_DIR, dirent->d_name);
		if (len < 0 || len >= (int)sizeof path) {
			keyd_log("CONFIG: y{WARNING} config path too long: %s\n", dirent->d_name);
			failed = 1;
			continue;
		}

		struct config_ent *ent = calloc(1, sizeof(*ent));
		if (!ent) {
			perror("calloc");
			failed = 1;
			break;
		}
		keyd_log("CONFIG: parsing b{%s}\n", path);
		if (config_parse(&ent->config, path) != 0) {
			free(ent);
			keyd_log("CONFIG: y{WARNING} failed to validate %s\n", path);
			failed = 1;
			continue;
		}

		struct output output = {
			.send_key = send_key,
			.on_layer_change = on_layer_change,
		};
		ent->kbd = new_keyboard(&ent->config, &output);
		if (!ent->kbd) {
			perror("calloc");
			free(ent);
			failed = 1;
			break;
		}
		ent->next = candidates;
		candidates = ent;
	}

	if (closedir(dh) < 0) {
		perror("closedir");
		failed = 1;
	}
	if (failed) {
		free_configs(candidates);
		return -1;
	}
	*result = candidates;
	return 0;
}

static struct config_ent *lookup_config_ent(const char *id, uint8_t flags)
{
	struct config_ent *ent = configs;
	struct config_ent *match = NULL;
	int rank = 0;

	while (ent) {
		int r = config_check_match(&ent->config, id, flags);

		if (r > rank) {
			match = ent;
			rank = r;
		}

		ent = ent->next;
	}

	/* The wildcard should not match mice or trackpads. */
	if (rank == 1 && !((flags & ID_KEYBOARD) && !(flags & ID_TRACKPAD))) {
		return NULL;
	} else {
		if (flags & ID_TRACKPAD)
			keyd_log("y{WARNING}: %s appears to be a trackpad, which is current unsupported. Mouse movement is likely to break(YMMV)\n", id);

		return match;
	}
}

static void manage_device(struct device *dev)
{
	uint8_t flags = 0;
	struct config_ent *ent;

	if (dev->is_virtual)
		return;

	if (dev->capabilities & CAP_KEY)
		flags |= ID_KEY;
	if (dev->capabilities & CAP_KEYBOARD)
		flags |= ID_KEYBOARD;
	if (dev->capabilities & CAP_MOUSE_ABS)
		flags |= ID_TRACKPAD;
	if (dev->capabilities & CAP_MOUSE)
		flags |= ID_MOUSE;

	if ((ent = lookup_config_ent(dev->id, flags))) {
		if (device_grab(dev)) {
			keyd_log("DEVICE: y{WARNING} Failed to grab %s\n", dev->path);
			dev->data = NULL;
			return;
		}

		keyd_log("DEVICE: g{match}    %s  %s\t(%s)\n",
			  dev->id, ent->config.path, dev->name);

		dev->data = ent->kbd;
	} else {
		dev->data = NULL;
		device_ungrab(dev);
		keyd_log("DEVICE: r{ignoring} %s  (%s)\n", dev->id, dev->name);
	}
}

static int reload(void)
{
	struct config_ent *candidates, *previous;
	size_t i;

	config_reload_deadline = 0;

	if (load_configs(&candidates) < 0) {
		/* Keep watching failed paths so a later edit can recover. */
		configwatch_refresh();
		keyd_log("CONFIG: y{WARNING} reload rejected; keeping previous configuration\n");
		return -1;
	}

	previous = configs;
	configs = candidates;
	configwatch_refresh();

	for (i = 0; i < device_table_sz; i++)
		manage_device(&device_table[i]);

	active_kbd = NULL;
	clear_vkbd();
	free_configs(previous);
	return 0;
}

static void send_success(int con)
{
	struct ipc_message msg = {0};

	msg.type = IPC_SUCCESS;;
	msg.sz = 0;

	xwrite(con, &msg, sizeof msg);
	close(con);
}

static void send_fail(int con, const char *fmt, ...)
{
	struct ipc_message msg = {0};
	va_list args;

	va_start(args, fmt);

	msg.type = IPC_FAIL;
	msg.sz = vsnprintf(msg.data, sizeof(msg.data), fmt, args);

	xwrite(con, &msg, sizeof msg);
	close(con);

	va_end(args);
}

static int input(char *buf, size_t sz, uint32_t timeout)
{
	size_t i;
	uint32_t codepoint;
	uint8_t codes[4];

	int csz;

	while ((csz = utf8_read_char(buf, &codepoint))) {
		if (csz < 0) {
			err("invalid UTF-8 in input");
			return -1;
		}
		int found = 0;
		char s[2];

		if (csz == 1) {
			uint8_t code, mods;
			s[0] = (char)codepoint;
			s[1] = 0;

			found = 1;
			if (!parse_key_sequence(s, &code, &mods)) {
				if (mods & MOD_SHIFT) {
					vkbd_send_key(vkbd, KEYD_LEFTSHIFT, 1);
					vkbd_send_key(vkbd, code, 1);
					vkbd_send_key(vkbd, code, 0);
					vkbd_send_key(vkbd, KEYD_LEFTSHIFT, 0);
				} else {
					vkbd_send_key(vkbd, code, 1);
					vkbd_send_key(vkbd, code, 0);
				}
			} else if ((char)codepoint == ' ') {
				vkbd_send_key(vkbd, KEYD_SPACE, 1);
				vkbd_send_key(vkbd, KEYD_SPACE, 0);
			} else if ((char)codepoint == '\n') {
				vkbd_send_key(vkbd, KEYD_ENTER, 1);
				vkbd_send_key(vkbd, KEYD_ENTER, 0);
			} else if ((char)codepoint == '\t') {
				vkbd_send_key(vkbd, KEYD_TAB, 1);
				vkbd_send_key(vkbd, KEYD_TAB, 0);
			} else {
				found = 0;
			}
		}

		if (!found) {
			int idx = unicode_lookup_index(codepoint);
			if (idx < 0) {
				err("ERROR: could not find code for \"%.*s\"", csz, buf);
				return -1;
			}

			unicode_get_sequence(idx, codes);

			for (i = 0; i < 4; i++) {
				vkbd_send_key(vkbd, codes[i], 1);
				vkbd_send_key(vkbd, codes[i], 0);
			}
		}
		buf+=csz;

		if (timeout)
			usleep(timeout);
	}

	return 0;
}

static void handle_client(int con)
{
	struct ipc_message msg;

	xread(con, &msg, sizeof msg);

	if (msg.sz >= sizeof(msg.data)) {
		send_fail(con, "maximum message size exceeded");
		return;
	}
	msg.data[msg.sz] = 0;

	if (msg.timeout > 1000000) {
		send_fail(con, "timeout cannot exceed 1000 ms");
		return;
	}

	switch (msg.type) {
		struct config_ent *ent;
		int success;
		struct macro macro;

	case IPC_MACRO:
		while (msg.sz && msg.data[msg.sz-1] == '\n')
			msg.data[--msg.sz] = 0;

		if (macro_parse(msg.data, &macro)) {
			send_fail(con, "%s", errstr);
			return;
		}

		macro_execute(send_key_macro_wrapper, NULL, &macro, msg.timeout);
		send_success(con);

		break;
	case IPC_INPUT:
		if (input(msg.data, msg.sz, msg.timeout))
			send_fail(con, "%s", errstr);
		else
			send_success(con);
		break;
	case IPC_RELOAD:
		if (reload() < 0)
			send_fail(con, "configuration reload failed; keeping previous configuration");
		else
			send_success(con);
		break;
	case IPC_LAYER_LISTEN:
		add_listener(con);
		break;
	case IPC_BIND:
		success = 0;

		if (msg.sz == sizeof(msg.data)) {
			send_fail(con, "bind expression size exceeded");
			return;
		}

		msg.data[msg.sz] = 0;

		for (ent = configs; ent; ent = ent->next) {
			if (!kbd_eval(ent->kbd, msg.data))
				success = 1;
		}

		if (success)
			send_success(con);
		else
			send_fail(con, "%s", errstr);


		break;
	default:
		send_fail(con, "Unknown command");
		break;
	}
}

static long process_keypress(struct keyboard *kbd, uint8_t code, int timestamp)
{
	struct key_event kev = {
		.code = code,
		.pressed = 1,
		.timestamp = timestamp
	};

	kbd_process_events(kbd, &kev, 1);

	kev.pressed = 0;
	return kbd_process_events(kbd, &kev, 1);
}

static int event_handler(struct event *ev)
{
	static int last_time = 0;
	static int timeout = 0;
	struct key_event kev = {0};

	timeout -= ev->timestamp - last_time;
	last_time = ev->timestamp;

	timeout = timeout < 0 ? 0 : timeout;

	switch (ev->type) {
	case EV_TIMEOUT:
		if (config_reload_deadline &&
		    configwatch_now() >= config_reload_deadline)
			reload();

		if (!active_kbd) {
			timeout = 0;
			break;
		}

		kev.code = 0;
		kev.timestamp = ev->timestamp;

		timeout = kbd_process_events(active_kbd, &kev, 1);
		break;
	case EV_DEV_EVENT:
		if (ev->dev->data) {
			struct keyboard *kbd = ev->dev->data;
			active_kbd = ev->dev->data;
			switch (ev->devev->type) {
			size_t i;
			case DEV_KEY:
				dbg("input %s %s", KEY_NAME(ev->devev->code), ev->devev->pressed ? "down" : "up");

				kev.code = ev->devev->code;
				kev.pressed = ev->devev->pressed;
				kev.timestamp = ev->timestamp;

				timeout = kbd_process_events(kbd, &kev, 1);
				break;
			case DEV_MOUSE_MOVE:
				if (kbd->scroll.active) {
					if (kbd->scroll.sensitivity == 0)
						break;
					int xticks, yticks;

					kbd->scroll.y += ev->devev->y;
					kbd->scroll.x += ev->devev->x;

					yticks = kbd->scroll.y / kbd->scroll.sensitivity;
					kbd->scroll.y %= kbd->scroll.sensitivity;

					xticks = kbd->scroll.x / kbd->scroll.sensitivity;
					kbd->scroll.x %= kbd->scroll.sensitivity;

					vkbd_mouse_scroll(vkbd, 0, -1*yticks);
					vkbd_mouse_scroll(vkbd, 0, xticks);
				} else {
					vkbd_mouse_move(vkbd, ev->devev->x, ev->devev->y);
				}
				break;
			case DEV_MOUSE_MOVE_ABS:
				vkbd_mouse_move_abs(vkbd, ev->devev->x, ev->devev->y);
				break;
			case DEV_MOUSE_SCROLL:
				if (active_kbd) {
					if (ev->devev->x > 0)
						for (i = 0;i < (size_t)ev->devev->x; i++)
							timeout = process_keypress(active_kbd, KEYD_SCROLL_RIGHT, ev->timestamp);
					if (ev->devev->x < 0)
						for (i = 0;i < (size_t)-1*ev->devev->x; i++)
							timeout = process_keypress(active_kbd, KEYD_SCROLL_LEFT, ev->timestamp);
					if (ev->devev->y > 0)
						for (i = 0;i < (size_t)ev->devev->y; i++)
							timeout = process_keypress(active_kbd, KEYD_SCROLL_UP, ev->timestamp);
					if (ev->devev->y < 0)
						for (i = 0;i < (size_t)-1*ev->devev->y; i++)
							timeout = process_keypress(active_kbd, KEYD_SCROLL_DOWN, ev->timestamp);
				}
				break;
			default:
				break;
			}
		} else if (!ev->dev->is_virtual && ev->dev->capabilities & CAP_MOUSE) {
			if (active_kbd && (ev->devev->type == DEV_KEY || ev->devev->type == DEV_MOUSE_SCROLL))
				timeout = process_keypress(active_kbd, KEYD_EXTERNAL_MOUSE_BUTTON, ev->timestamp);
		} else if (ev->dev->is_virtual && ev->devev->type == DEV_LED) {
			size_t i;

			/* 
			 * Propagate LED events received by the virtual device from userspace
			 * to all grabbed devices.
			 *
			 * NOTE/TODO: Account for potential layer_indicator interference
			 */
			for (i = 0; i < device_table_sz; i++)
				if (device_table[i].data)
					device_set_led(&device_table[i], ev->devev->code, ev->devev->pressed);
		}

		break;
	case EV_DEV_ADD:
		manage_device(ev->dev);
		break;
	case EV_DEV_REMOVE:
		keyd_log("DEVICE: r{removed}\t%s %s\n", ev->dev->id, ev->dev->name);

		break;
	case EV_FD_ACTIVITY:
		if (ev->fd == ipcfd) {
			int con = accept(ipcfd, NULL, 0);
			if (con < 0) {
				perror("accept");
				exit(-1);
			}

			handle_client(con);
		} else if (ev->fd == configfd) {
			configwatch_handle();
		}
		break;
	default:
		break;
	}

	if (config_reload_deadline) {
		long rem = config_reload_deadline - configwatch_now();

		/* A pending reload must never be mistaken for the 0 == infinite timeout. */
		if (rem <= 0)
			rem = 1;
		if (timeout <= 0 || rem < timeout)
			timeout = (int)rem;
	}

	return timeout;
}

int run_daemon(int argc, char *argv[])
{
	struct sched_param sp;
	ipcfd = ipc_create_server();

	if (ipcfd < 0)
		die("failed to create %s (another instance already running?)", SOCKET_PATH);

	vkbd = vkbd_init(VKBD_NAME);

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	sp.sched_priority = 49;
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) {
		perror("pthread_setschedparam");
		exit(-1);
	}

	if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
		perror("mlockall");
		exit(-1);
	}

	configfd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (configfd < 0) {
		perror("inotify_init1");
		exit(-1);
	}

	evloop_add_fd(ipcfd);
	evloop_add_fd(configfd);

	reload();

	atexit(cleanup);

	keyd_log("Starting keyd "VERSION"\n");
	evloop(event_handler);

	return 0;
}
