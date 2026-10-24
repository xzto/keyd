#include <stdio.h>
#include <stdlib.h>
#include "../src/keyd.h"

static struct config config;
static struct keyboard *kbd;
static struct key_event output[128];
static size_t noutput;
static long now;

static void send_key(uint8_t code, uint8_t pressed)
{
	assert(noutput < ARRAY_SIZE(output));
	output[noutput++] = (struct key_event) { .code = code, .pressed = pressed };
}

static void on_layer_change(const struct keyboard *keyboard,
			    const struct layer *layer, uint8_t active)
{
}

static void reset(void)
{
	struct output out = { .send_key = send_key, .on_layer_change = on_layer_change };
	free(kbd);
	kbd = new_keyboard(&config, &out);
	noutput = 0;
	now = 1000;
}

static long event(uint8_t code, uint8_t pressed, long delay)
{
	struct key_event ev = { .code = code, .pressed = pressed };
	now += delay;
	ev.timestamp = now;
	return kbd_process_events(kbd, &ev, 1);
}

#define DOWN(code) event(KEYD_##code, 1, 1)
#define UP(code) event(KEYD_##code, 0, 1)
#define TAP(code) do { DOWN(code); UP(code); } while (0)
#define WAIT(ms) event(0, 0, ms)
#define EXPECT(...) do { \
	const int expected[] = { __VA_ARGS__ }; \
	expect(expected, ARRAY_SIZE(expected), __LINE__); \
} while (0)

static void expect(const int *expected, size_t n, int line)
{
	if (noutput != n) {
		fprintf(stderr, "line %d: expected %zu events, got %zu\n", line, n, noutput);
		for (size_t i = 0; i < noutput; i++)
			fprintf(stderr, "%s%s ", output[i].pressed ? "+" : "-", KEY_NAME(output[i].code));
		fputc('\n', stderr);
		abort();
	}
	for (size_t i = 0; i < n; i++) {
		int actual = output[i].pressed ? output[i].code : -output[i].code;
		if (actual != expected[i]) {
			fprintf(stderr, "line %d, event %zu: expected %d, got %d\n",
				line, i, expected[i], actual);
			abort();
		}
	}
	noutput = 0;
}

static int active(const char *name)
{
	int i = config_get_layer_index(&kbd->config, name);
	assert(i >= 0);
	return kbd->layer_state[i].active;
}

static void test_magic_caps(void)
{
	for (int reverse = 0; reverse < 2; reverse++) {
		reset();
		assert(!kbd->active_virtual_layers);
		if (reverse) {
			TAP(F1);
			DOWN(F2);
		} else {
			DOWN(F2);
			TAP(F1);
		}
		TAP(A);
		EXPECT(KEYD_MINUS, -KEYD_MINUS);
		assert(active("magic-caps"));
		UP(F2);
		TAP(MINUS);
		EXPECT(KEYD_MINUS, -KEYD_MINUS);
		assert(active("magic-caps"));
		TAP(A);
		EXPECT(KEYD_LEFTSHIFT, KEYD_A, -KEYD_A, -KEYD_LEFTSHIFT);
		assert(active("magic-caps"));
		TAP(B);
		EXPECT(KEYD_B, -KEYD_B);
		assert(!active("magic-caps"));
		assert(!kbd->active_virtual_layers);
	}
}

static void test_modifiers(void)
{
	const uint8_t mods[] = { KEYD_LEFTCTRL, KEYD_LEFTSHIFT, KEYD_LEFTALT, KEYD_LEFTMETA };
	for (size_t i = 0; i < ARRAY_SIZE(mods); i++) {
		reset();
		TAP(F1);
		event(mods[i], 1, 1);
		TAP(MINUS);
		event(mods[i], 0, 1);
		int expected[] = { mods[i], KEYD_MINUS, -KEYD_MINUS, -mods[i] };
		expect(expected, ARRAY_SIZE(expected), __LINE__);
		assert(active("magic-caps"));
	}

	reset();
	TAP(F1);
	TAP(Q);
	EXPECT(KEYD_LEFTSHIFT, KEYD_MINUS, -KEYD_MINUS, -KEYD_LEFTSHIFT);
	assert(!active("magic-caps"));
	TAP(F1);
	TAP(W);
	EXPECT(KEYD_LEFTCTRL, KEYD_MINUS, -KEYD_MINUS, -KEYD_LEFTCTRL);
	assert(!active("magic-caps"));

	DOWN(F7);
	TAP(Q);
	UP(F7);
	EXPECT(KEYD_Z, -KEYD_Z);

	reset();
	DOWN(F6);
	EXPECT(KEYD_LEFTSHIFT);
	TAP(F1);
	TAP(A);
	EXPECT(-KEYD_LEFTSHIFT, KEYD_MINUS, -KEYD_MINUS, KEYD_LEFTSHIFT, -KEYD_LEFTSHIFT);
	assert(active("magic-caps"));
	TAP(B);
	EXPECT(KEYD_LEFTSHIFT, KEYD_B, -KEYD_B);
	UP(F6);
	EXPECT(-KEYD_LEFTSHIFT);
}

static void test_precedence_and_cache(void)
{
	reset();
	DOWN(F3); DOWN(F4);
	TAP(MINUS);
	EXPECT(KEYD_C, -KEYD_C);
	DOWN(F2);
	TAP(A);
	EXPECT(KEYD_C, -KEYD_C);
	UP(F4);
	TAP(A);
	EXPECT(KEYD_B, -KEYD_B);
	TAP(V);
	EXPECT(KEYD_B, -KEYD_B);
	UP(F2);
	DOWN(MINUS);
	EXPECT(KEYD_B, -KEYD_B);
	UP(F3);
	assert(!kbd->active_virtual_layers);
	UP(MINUS);
	assert(active("keep"));
	TAP(N);
	EXPECT(KEYD_N, -KEYD_N);
	assert(!active("keep"));

	reset();
	assert(kbd_eval(kbd, "first.virtual n = oneshotm(keep, b)") == 0);
	assert(kbd_eval(kbd, "second.virtual n = oneshotm(keep, c)") == 0);
	DOWN(F3); DOWN(F4); TAP(N); UP(F4); UP(F3);
	EXPECT(KEYD_C, -KEYD_C);
	DOWN(F4); DOWN(F3); TAP(N); UP(F3); UP(F4);
	EXPECT(KEYD_B, -KEYD_B);

	reset();
	DOWN(F4); DOWN(F3);
	kbd->layer_state[config_get_layer_index(&kbd->config, "first")].activation_time =
		kbd->layer_state[config_get_layer_index(&kbd->config, "second")].activation_time;
	TAP(MINUS);
	EXPECT(KEYD_C, -KEYD_C);
	UP(F3); UP(F4);

	reset();
	TAP(F5);
	TAP(MINUS);
	EXPECT(KEYD_B, -KEYD_B);
	TAP(F5);
	assert(!kbd->active_virtual_layers);
}

static void test_runtime_and_scope(void)
{
	reset();
	TAP(MINUS);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);
	assert(kbd_eval(kbd, "virtual minus = oneshotm(keep, b)") == 0);
	assert(kbd->active_virtual_layers);
	TAP(MINUS);
	EXPECT(KEYD_B, -KEYD_B);
	assert(kbd_eval(kbd, "virtual b = oneshotm(keep, minus)") == 0);
	TAP(MINUS); TAP(B);
	EXPECT(KEYD_B, -KEYD_B, KEYD_MINUS, -KEYD_MINUS);
	assert(kbd_eval(kbd, "virtual minus = oneshotm(keep, c)") == 0);
	assert(kbd->config.layers[0].nr_virtual_bindings == 2);
	TAP(MINUS);
	EXPECT(KEYD_C, -KEYD_C);
	assert(kbd_eval(kbd, "reset") == 0);
	assert(!kbd->active_virtual_layers);

	DOWN(F3);
	TAP(F10);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);
	DOWN(F11); WAIT(20); UP(F11);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);
	TAP(X);
	EXPECT();
	UP(F3);

	TAP(F8);
	assert(kbd->active_virtual_layers);
	TAP(A);
	EXPECT(KEYD_Y, -KEYD_Y);
	TAP(F9);
	assert(!kbd->active_virtual_layers);
	TAP(A);
	EXPECT(KEYD_A, -KEYD_A);
}

static void test_macro_and_nested_output(void)
{
	reset();
	DOWN(F2); DOWN(F3);
	assert(kbd_eval(kbd, "first.virtual 0 = oneshotm(keep, c)") == 0);
	assert(kbd_eval(kbd, "first.virtual equal = oneshotm(keep, d)") == 0);
	TAP(Z);
	EXPECT(KEYD_0, -KEYD_0, KEYD_MINUS, -KEYD_MINUS, KEYD_EQUAL, -KEYD_EQUAL);
	assert(!active("keep"));
	TAP(A);
	EXPECT(KEYD_B, -KEYD_B);
	assert(active("keep"));
	UP(F3); UP(F2);

	reset();
	assert(kbd_eval(kbd, "magic-caps.virtual 0 = oneshotm(magic-caps, 0)") == 0);
	DOWN(F2);
	TAP(F1);
	TAP(Z);
	EXPECT(KEYD_0, -KEYD_0, KEYD_MINUS, -KEYD_MINUS, KEYD_EQUAL, -KEYD_EQUAL);
	assert(!active("magic-caps"));
	assert(!kbd->active_virtual_layers);
	UP(F2);

	TAP(F1);
	TAP(F10);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);
	assert(!active("magic-caps"));

	TAP(F1);
	DOWN(F11); WAIT(20); UP(F11);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);
	assert(!active("magic-caps"));
}

static void test_lifecycle(void)
{
	reset();
	TAP(F1);
	DOWN(G); DOWN(H); UP(H); UP(G);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);
	assert(active("magic-caps"));

	assert(kbd_eval(kbd, "main.esc = clear()") == 0);
	TAP(ESC);
	assert(!kbd->active_virtual_layers);

	kbd->config.oneshot_timeout = 10;
	TAP(F1);
	assert(kbd->active_virtual_layers);
	WAIT(11);
	assert(!kbd->active_virtual_layers);

	strcpy(config.default_layout, "alternate");
	reset();
	assert(kbd->active_virtual_layers);
	TAP(A);
	EXPECT(KEYD_Y, -KEYD_Y);
	config.default_layout[0] = 0;
}

static void test_dot_bindings(void)
{
	const char *bindings[] = {
		"virtual . = oneshotm(keep, b)",
		"virtual\t. = oneshotm(keep, b)",
		"main.virtual . = oneshotm(keep, b)",
		"virtual \t  . = oneshotm(keep, b)",
		"main.virtual\t \t. = oneshotm(keep, b)",
		". = b",
		"main.. = b",
	};

	for (size_t i = 0; i < ARRAY_SIZE(bindings); i++) {
		reset();
		assert(kbd_eval(kbd, bindings[i]) == 0);
		TAP(DOT);
		EXPECT(KEYD_B, -KEYD_B);
	}

	reset();
	assert(kbd_eval(kbd, "virtual dot = oneshotm(keep, .)") == 0);
	TAP(DOT);
	EXPECT(KEYD_DOT, -KEYD_DOT);
	assert(kbd_eval(kbd, "virtual S-. = oneshotm(keep, x)") == 0);
	assert(kbd_eval(kbd, "main.q = S-dot") == 0);
	TAP(Q);
	EXPECT(KEYD_X, -KEYD_X);
}

static void test_parser_restrictions(void)
{
	const char *invalid[] = {
		"minus", "swap(keep)", "swapm(keep, minus)", "oneshot(keep)",
		"overloadt(control, minus, 20)", "overloadi(minus, minus, 50)",
		"timeout(minus, 20, minus)", "macro(minus)", "command(true)",
		"oneshotm(keep, macro(a b))", "oneshotm(keep, macro(a+b))",
		"oneshotm(keep, macro())", "oneshotm(keep, macro(10ms minus))",
		"oneshotm(keep, é)", "oneshotm(main, minus)",
		"oneshotm(alternate, minus)", "oneshotm(missing, minus)",
		"oneshotm(keep)", "oneshotm(keep, minus, a)", "unbind", "",
	};

	reset();
	assert(kbd_eval(kbd, "virtual minus = oneshotm(keep, b)") == 0);
	for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
		char entry[MAX_EXP_LEN];
		size_t macros = kbd->config.nr_macros;
		size_t descriptors = kbd->config.nr_descriptors;
		size_t commands = kbd->config.nr_commands;
		snprintf(entry, sizeof entry, "virtual minus = %s", invalid[i]);
		assert(kbd_eval(kbd, entry) < 0);
		assert(kbd->config.nr_macros == macros);
		assert(kbd->config.nr_descriptors == descriptors);
		assert(kbd->config.nr_commands == commands);
		assert(kbd->config.layers[0].nr_virtual_bindings == 1);
		TAP(MINUS);
		EXPECT(KEYD_B, -KEYD_B);
	}

	size_t macros = kbd->config.nr_macros;
	uint32_t layers = kbd->config.virtual_layers;
	assert(kbd_eval(kbd, "first+symbols.virtual minus = oneshotm(keep, c)") < 0);
	assert(kbd->config.nr_macros == macros);
	assert(kbd->config.virtual_layers == layers);
	assert(!kbd->config.layers[config_get_layer_index(&kbd->config, "first+symbols")].nr_virtual_bindings);

	assert(kbd_eval(kbd, "virtual a+b = oneshotm(keep, c)") < 0);
	assert(kbd_eval(kbd, "virtual invalid = oneshotm(keep, c)") < 0);
	assert(kbd_eval(kbd, "main.remap minus = oneshotm(keep, c)") < 0);
	assert(kbd_eval(kbd, "virtual = = oneshotm(keep, c)") == 0);
	TAP(EQUAL);
	EXPECT(KEYD_C, -KEYD_C);
	assert(kbd_eval(kbd, "virtual minus = oneshotm(keep, macro(C-minus))") == 0);
	TAP(MINUS);
	EXPECT(KEYD_LEFTCTRL, KEYD_MINUS, -KEYD_MINUS, -KEYD_LEFTCTRL);

	kbd->config.nr_macros = ARRAY_SIZE(kbd->config.macros);
	assert(kbd_eval(kbd, "virtual minus = oneshotm(keep, c)") < 0);
	assert(kbd->config.layers[0].nr_virtual_bindings == 2);
}

int main(int argc, char **argv)
{
	if (argc != 2 || config_parse(&config, argv[1]))
		return 1;
	test_magic_caps();
	test_modifiers();
	test_precedence_and_cache();
	test_runtime_and_scope();
	test_macro_and_nested_output();
	test_lifecycle();
	test_dot_bindings();
	test_parser_restrictions();
	free(kbd);
	puts("Virtual binding tests PASSED");
	return 0;
}
