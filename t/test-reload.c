#include "../src/keyd.h"

static char test_config_dir[PATH_MAX] = "/tmp/keyd-reload-test-XXXXXX";
#define CONFIG_DIR test_config_dir
#define VERSION "test"
#define SOCKET_PATH "/unused-keyd-reload-test.socket"
#include "../src/daemon.c"

struct device device_table[MAX_DEVICES];
size_t device_table_sz = 1;
static int grabs, ungrabs;
static int output[256];
static size_t noutput;
static int now = 1000;

int device_grab(struct device *dev)
{
	grabs++;
	dev->grabbed = 1;
	return 0;
}

int device_ungrab(struct device *dev)
{
	ungrabs++;
	dev->grabbed = 0;
	return 0;
}

void device_set_led(const struct device *dev, int led, int state) {}
void vkbd_mouse_move(const struct vkbd *kbd, int x, int y) {}
void vkbd_mouse_move_abs(const struct vkbd *kbd, int x, int y) {}
void vkbd_mouse_scroll(const struct vkbd *kbd, int x, int y) {}
void free_vkbd(struct vkbd *kbd) {}

void vkbd_send_key(const struct vkbd *kbd, uint8_t code, int state)
{
	assert(noutput < ARRAY_SIZE(output));
	output[noutput++] = state ? code : -code;
}

static void path_for(const char *name, char path[PATH_MAX])
{
	int n = snprintf(path, PATH_MAX, "%s/%s", CONFIG_DIR, name);
	assert(n > 0 && n < PATH_MAX);
}

static void write_file(const char *name, const char *content)
{
	char path[PATH_MAX];
	path_for(name, path);
	FILE *fh = fopen(path, "w");
	assert(fh);
	assert(fputs(content, fh) >= 0);
	assert(fclose(fh) == 0);
}

static void remove_file(const char *name)
{
	char path[PATH_MAX];
	path_for(name, path);
	assert(unlink(path) == 0);
}

static void key(uint8_t code, int pressed)
{
	struct device_event kev = { .type = DEV_KEY, .code = code, .pressed = pressed };
	struct event ev = {
		.type = EV_DEV_EVENT,
		.dev = &device_table[0],
		.devev = &kev,
		.timestamp = ++now,
	};
	event_handler(&ev);
}

static void autoreload(void)
{
	configwatch_handle();
	assert(config_reload_deadline > 0);
	config_reload_deadline = configwatch_now();
	struct event ev = { .type = EV_TIMEOUT, .timestamp = now };
	event_handler(&ev);
	assert(!config_reload_deadline);
	assert(nr_config_watches > 0);
}

static int manual_reload(void)
{
	int sockets[2];
	struct ipc_message message = { .type = IPC_RELOAD };
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	xwrite(sockets[0], &message, sizeof message);
	handle_client(sockets[1]);
	xread(sockets[0], &message, sizeof message);
	close(sockets[0]);
	if (message.type == IPC_FAIL)
		assert(strstr(message.data, "keeping previous configuration"));
	return message.type;
}

static const char original_config[] =
	"[ids]\n*\n[main]\na = b\n"
	"f1 = oneshot(keep)\nf3 = overloadt2(meta, space, 100)\nz+x = c\n[keep]\n";
static const char replacement_config[] = "[ids]\n*\n[main]\na = x\n";

struct snapshot {
	struct config_ent *configs;
	struct keyboard *kbd;
	struct keyboard state;
	uint8_t keys[256];
	int grabs, ungrabs;
	size_t noutput;
};

static struct snapshot saved;

static void save_state(void)
{
	saved.configs = configs;
	saved.kbd = device_table[0].data;
	saved.state = *saved.kbd;
	memcpy(saved.keys, keystate, sizeof keystate);
	saved.grabs = grabs;
	saved.ungrabs = ungrabs;
	saved.noutput = noutput;
}

static void expect_preserved(void)
{
	assert(configs == saved.configs);
	assert(device_table[0].data == saved.kbd);
	assert(active_kbd == saved.kbd);
	struct keyboard *kbd = saved.kbd;
	assert(!memcmp(&kbd->config, &saved.state.config, sizeof(kbd->config)));
	assert(!memcmp(kbd->cache, saved.state.cache, sizeof(kbd->cache)));
	assert(!memcmp(kbd->layer_state, saved.state.layer_state, sizeof(kbd->layer_state)));
	assert(!memcmp(&kbd->pending_timeout, &saved.state.pending_timeout, sizeof(kbd->pending_timeout)));
	assert(!memcmp(&kbd->pending_overload, &saved.state.pending_overload, sizeof(kbd->pending_overload)));
	assert(!memcmp(&kbd->chord, &saved.state.chord, sizeof(kbd->chord)));
	assert(!memcmp(kbd->keystate, saved.state.keystate, sizeof(kbd->keystate)));
	assert(kbd->nr_timeouts == saved.state.nr_timeouts);
	assert(!memcmp(kbd->timeouts, saved.state.timeouts, kbd->nr_timeouts * sizeof(kbd->timeouts[0])));
	assert(kbd->oneshot_latch == saved.state.oneshot_latch);
	assert(!memcmp(keystate, saved.keys, sizeof keystate));
	assert(grabs == saved.grabs && ungrabs == saved.ungrabs);
	assert(noutput == saved.noutput);
}

static void test_failed_reloads(void)
{
	const char *invalid[] = {
		"[ids]\n*\n[main]\na = unknown-action\ns = d\n",
		"invalid header\n[ids]\n*\n[main]\na = x\n",
		"[ids]\n*\n[main]\ninclude missing-include\na = x\n",
		"",
		"# empty configuration\n",
		"[main]\na = x\n",
	};
	for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
		write_file("default.conf", invalid[i]);
		autoreload();
		expect_preserved();
	}
	assert(manual_reload() == IPC_FAIL);
	expect_preserved();

	char path[PATH_MAX];
	path_for("default.conf", path);
	remove_file("default.conf");
	assert(symlink("missing-target", path) == 0);
	autoreload();
	expect_preserved();
	remove_file("default.conf");

	write_file("default.conf", replacement_config);
	write_file("other.conf", "[ids]\nother-device\n[main]\ns = unknown-action\n");
	autoreload();
	expect_preserved();

	char moved[PATH_MAX];
	int n = snprintf(moved, sizeof moved, "%s-moved", CONFIG_DIR);
	assert(n > 0 && (size_t)n < sizeof moved);
	assert(rename(CONFIG_DIR, moved) == 0);
	assert(reload() < 0);
	expect_preserved();
	assert(rename(moved, CONFIG_DIR) == 0);
}

static void test_read_errors(void)
{
	static struct config probe;
	char directory[PATH_MAX];
	path_for("include-dir", directory);
	assert(mkdir(directory, 0700) == 0);
	write_file("default.conf", "[ids]\n*\n[main]\ninclude include-dir\na = x\n");
	assert(config_parse(&probe, directory) != 0);
	char path[PATH_MAX];
	path_for("default.conf", path);
	assert(config_parse(&probe, path) != 0);
	autoreload();
	expect_preserved();
	assert(rmdir(directory) == 0);
}

static void test_recovery_and_removal(void)
{
	write_file("tap-keys", "a = x\n");
	write_file("default.conf", "[ids]\n*\n[main]\ninclude tap-keys\n");
	write_file("other.conf", "[ids]\nother-device\n[main]\ns = d\n");
	autoreload();
	assert(configs != saved.configs);
	assert(device_table[0].data != saved.kbd);
	assert(active_kbd == NULL);
	assert(!keystate[KEYD_B] && !keystate[KEYD_LEFTCTRL]);
	assert(noutput == saved.noutput + 2);
	struct keyboard *kbd = device_table[0].data;
	assert(kbd->config.layers[0].keymap[KEYD_A].args[0].code == KEYD_X);
	assert(!kbd->pending_overload.code);
	assert(kbd->layer_state[config_get_layer_index(&kbd->config, "control")].active == 0);
	assert(manual_reload() == IPC_SUCCESS);

	noutput = 0;
	key(KEYD_A, 0);
	key(KEYD_LEFTCTRL, 0);
	key(KEYD_F3, 0);
	key(KEYD_Z, 0);
	key(KEYD_A, 1);
	key(KEYD_A, 0);
	assert(noutput == 2 && output[0] == KEYD_X && output[1] == -KEYD_X);

	remove_file("default.conf");
	remove_file("other.conf");
	remove_file("tap-keys");
	assert(manual_reload() == IPC_SUCCESS);
	assert(configs == NULL && device_table[0].data == NULL);
	assert(!device_table[0].grabbed);

	write_file("default.conf", "[ids]\n*\n[main]\na = unknown-action\n");
	assert(manual_reload() == IPC_FAIL);
	assert(configs == NULL && device_table[0].data == NULL);
	write_file("default.conf", original_config);
	autoreload();
	assert(configs && device_table[0].data);
}

int main(void)
{
	assert(mkdtemp(test_config_dir));
	strcpy(device_table[0].id, "test-keyboard");
	device_table[0].capabilities = CAP_KEYBOARD | CAP_KEY;
	configfd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	assert(configfd >= 0);
	write_file("default.conf", original_config);
	assert(reload() == 0);
	assert(device_table[0].data && device_table[0].grabbed);

	key(KEYD_A, 1);
	key(KEYD_LEFTCTRL, 1);
	key(KEYD_F1, 1); key(KEYD_F1, 0);
	key(KEYD_F3, 1);
	key(KEYD_Z, 1);
	struct keyboard *kbd = device_table[0].data;
	assert(keystate[KEYD_B] && keystate[KEYD_LEFTCTRL]);
	assert(kbd->pending_overload.code == KEYD_F3);
	assert(kbd->chord.state == CHORD_PENDING_DISAMBIGUATION);
	assert(kbd->layer_state[config_get_layer_index(&kbd->config, "keep")].oneshot_depth == 1);
	save_state();
	test_failed_reloads();
	test_read_errors();
	test_recovery_and_removal();

	free_configs(configs);
	configs = NULL;
	close(configfd);
	configfd = -1;
	remove_file("default.conf");
	assert(rmdir(CONFIG_DIR) == 0);
	puts("Transactional reload tests PASSED");
	return 0;
}
