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

static void write_bytes(const char *name, const void *content, size_t size)
{
	char path[PATH_MAX];
	path_for(name, path);
	FILE *fh = fopen(path, "wb");
	assert(fh);
	assert(fwrite(content, 1, size, fh) == size);
	assert(fclose(fh) == 0);
}

static void write_file(const char *name, const char *content)
{
	write_bytes(name, content, strlen(content));
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
		"[ids]\n*\n[]\na = b\n",
		"[ids]\n*\n[:]\na = b\n",
		"[ids]\n*\n[main]\na = overload(a, b, c, d, e, f)\n",
		"[ids]\n*\n[main]\na = lettermod(meta, timeout(a, b, c, d, e, f), 150, 200)\n",
		"[ids]\n*\n[main]\ninherit a = lettermod(meta, inherit, 1, 2, 3, 4)\n",
		"[ids]\n*\n[main]\na = macro(\xF0)\n",
		"[ids]\n*\n[global]\noneshot_timeout\n",
		"[ids]\n*\n[aliases]\na\n",
		"[ids]\n*\n[main+main+main+main+main+main+main+main+main]\na = b\n",
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
	configwatch_handle();
	assert(!config_reload_deadline);
	assert(reload() < 0);
	expect_preserved();
	assert(rename(moved, CONFIG_DIR) == 0);
}

static char *repeat_text(const char *prefix, const char *part, size_t count, const char *suffix)
{
	size_t a = strlen(prefix), b = strlen(part), c = strlen(suffix);
	char *text = malloc(a + b * count + c + 1);
	assert(text);
	memcpy(text, prefix, a);
	for (size_t i = 0; i < count; i++)
		memcpy(text + a + b * i, part, b);
	memcpy(text + a + b * count, suffix, c + 1);
	return text;
}

static void reject_repeated(const char *prefix, const char *part, size_t count, const char *suffix)
{
	char *text = repeat_text(prefix, part, count, suffix);
	write_file("default.conf", text);
	free(text);
	autoreload();
	expect_preserved();
}

static void test_parser_limits(void)
{
	const char *header = "[ids]\n*\n[main]\n";
	reject_repeated("[ids]\n*\n", "[main]\n", 33, "");
	reject_repeated(header, "a = b\n", 1025, "");
	reject_repeated(header, "#\n", 4096, "");

	const char *ids[] = { "k:device\n", "m:device\n", "-device\n", "device\n" };
	for (size_t i = 0; i < ARRAY_SIZE(ids); i++)
		reject_repeated("[ids]\n", ids[i], 65, "[main]\na = b\n");

	char long_line[5002];
	memset(long_line, 'x', sizeof long_line);
	long_line[0] = '#';
	long_line[5000] = '\n';
	long_line[5001] = 0;
	reject_repeated(header, long_line, 1, "");

	char chunk[1027];
	memset(chunk, 'x', sizeof chunk);
	chunk[0] = '#';
	chunk[1025] = '\n';
	chunk[1026] = 0;
	reject_repeated(header, chunk, 65, "");

	write_file("shared", "");
	reject_repeated(header, "include shared\n", 129, "");
	char *text = repeat_text("", "#\n", 4096, "");
	write_file("shared", text);
	free(text);
	reject_repeated(header, "include shared\n", 1, "");
	text = repeat_text("", chunk, 65, "");
	write_file("shared", text);
	free(text);
	reject_repeated(header, "include shared\n", 1, "");
	write_file("shared", long_line);
	reject_repeated(header, "include shared\n", 1, "");
	remove_file("shared");

	char include_line[4090];
	memcpy(include_line, "include ", 8);
	memset(include_line + 8, 'x', 4080);
	include_line[4088] = '\n';
	include_line[4089] = 0;
	reject_repeated(header, include_line, 1, "");
	reject_repeated("[ids]\n*\n[", "x", 300, "]\na = b\n");
	reject_repeated("[ids]\n*\n[", "x", 65, "]\na = b\n");
	reject_repeated(header, "a = x", 120, "\n");

	const char embedded_nul[] = "[ids]\n*\n[main]\na = b\0s = d\n";
	write_bytes("default.conf", embedded_nul, sizeof embedded_nul - 1);
	autoreload();
	expect_preserved();

	const char *invalid_utf8[] = {
		"\x80", "\xC0\xAF", "\xC2", "\xE2\x82", "\xF0\x9F\x92",
		"\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
	};
	for (size_t i = 0; i < ARRAY_SIZE(invalid_utf8); i++) {
		assert(utf8_strlen(invalid_utf8[i]) < 0);
		char bytes[8];
		strcpy(bytes, invalid_utf8[i]);
		struct macro macro;
		assert(macro_parse(bytes, &macro) < 0);
		assert(input(bytes, strlen(bytes), 0) < 0);
		char config_text[128];
		snprintf(config_text, sizeof config_text, "%sa = macro(%s)\n", header, invalid_utf8[i]);
		reject_repeated("", config_text, 1, "");
	}
	uint32_t codepoint;
	assert(utf8_read_char("\xF0\x9F\x98\x80", &codepoint) == 4 && codepoint == 0x1F600);
	assert(utf8_strlen("é€😀") == 3);
	assert(parse_key_sequence("C-", NULL, NULL) < 0);
	uint8_t keycode, mods;
	assert(parse_key_sequence("C--", &keycode, &mods) == 0);
	assert(keycode == KEYD_MINUS && mods == MOD_CTRL);

	static struct config probe;
	char path[PATH_MAX];
	write_file("valid-utf8", "[ids]\n*\n[main]\n# é€😀\na = b\n");
	path_for("valid-utf8", path);
	assert(config_parse(&probe, path) == 0);
	remove_file("valid-utf8");

	path_for("pipe", path);
	assert(mkfifo(path, 0600) == 0);
	reject_repeated(header, "include pipe\n", 1, "");
	remove_file("pipe");
	remove_file("default.conf");
	path_for("default.conf", path);
	assert(mkfifo(path, 0600) == 0);
	autoreload();
	expect_preserved();
	remove_file("default.conf");

	char sibling[PATH_MAX], sibling_file[PATH_MAX], config_text[PATH_MAX + 64];
	int n = snprintf(sibling, sizeof sibling, "%s-sibling", CONFIG_DIR);
	assert(n > 0 && (size_t)n < sizeof sibling);
	assert(mkdir(sibling, 0700) == 0);
	n = snprintf(sibling_file, sizeof sibling_file, "%s/keys", sibling);
	assert(n > 0 && (size_t)n < sizeof sibling_file);
	FILE *fh = fopen(sibling_file, "w");
	assert(fh && fputs("a = x\n", fh) >= 0);
	assert(fclose(fh) == 0);
	n = snprintf(config_text, sizeof config_text, "%sinclude ../%s/keys\n",
		     header, strrchr(sibling, '/') + 1);
	assert(n > 0 && (size_t)n < sizeof config_text);
	reject_repeated("", config_text, 1, "");
	assert(unlink(sibling_file) == 0 && rmdir(sibling) == 0);

	write_file("default.conf", replacement_config);
	write_file("percent%n.conf", "[ids]\n*\n[main]\na = unknown-action\n");
	autoreload();
	expect_preserved();
	remove_file("percent%n.conf");
	write_file("percent%n", "a = unknown-action\n");
	reject_repeated(header, "include percent%n\n", 1, "");
	remove_file("percent%n");
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
	test_parser_limits();
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
