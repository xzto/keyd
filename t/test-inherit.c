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

static void test_tap_hold_and_repeat(void)
{
	reset();
	TAP(A);
	EXPECT(KEYD_A, -KEYD_A);
	DOWN(A);
	EXPECT(KEYD_A);
	assert(!kbd->pending_overload.code);
	WAIT(1000);
	EXPECT();
	assert(kbd->keystate[KEYD_A]);
	assert(!active("meta"));
	UP(A);
	EXPECT(-KEYD_A);

	WAIT(151);
	DOWN(A);
	EXPECT();
	WAIT(201);
	EXPECT(KEYD_LEFTMETA);
	assert(active("meta"));
	assert(!kbd->keystate[KEYD_A]);
	UP(A);
	EXPECT(-KEYD_LEFTMETA);

	reset();
	DOWN(F2);
	TAP(A);
	EXPECT(KEYD_1, -KEYD_1);
	DOWN(A);
	EXPECT(KEYD_1);
	WAIT(1000);
	EXPECT();
	assert(kbd->keystate[KEYD_1]);
	assert(!kbd->pending_overload.code);
	assert(!active("meta"));
	UP(A); UP(F2);
	EXPECT(-KEYD_1);
}

static void test_layers_and_noop(void)
{
	reset();
	DOWN(F2);
	TAP(A);
	EXPECT(KEYD_1, -KEYD_1);
	WAIT(151);
	DOWN(S);
	UP(F2);
	UP(S);
	EXPECT(KEYD_LEFTSHIFT, KEYD_MINUS, -KEYD_MINUS, -KEYD_LEFTSHIFT);

	reset();
	DOWN(F3);
	TAP(A);
	EXPECT();
	WAIT(151);
	DOWN(A); WAIT(201);
	EXPECT(KEYD_LEFTMETA);
	UP(A);
	EXPECT(-KEYD_LEFTMETA);
	TAP(S);
	EXPECT();
	WAIT(151);
	DOWN(S); WAIT(201);
	EXPECT(KEYD_LEFTALT);
	UP(S); UP(F3);
	EXPECT(-KEYD_LEFTALT);

	reset();
	DOWN(F2); DOWN(F3);
	TAP(A);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);
	UP(F3); UP(F2);

	reset();
	TAP(F7); TAP(A);
	EXPECT(KEYD_B, -KEYD_B);
	TAP(F8); TAP(A);
	EXPECT(KEYD_A, -KEYD_A);
}

static void test_overrides_and_modifiers(void)
{
	reset();
	DOWN(F4);
	TAP(A);
	EXPECT(KEYD_X, -KEYD_X);
	WAIT(151);
	DOWN(A); WAIT(251);
	EXPECT(KEYD_LEFTCTRL);
	assert(!active("meta"));
	UP(A);
	EXPECT(-KEYD_LEFTCTRL);
	TAP(S);
	EXPECT(KEYD_Y, -KEYD_Y);
	UP(F4);

	reset();
	DOWN(F5);
	EXPECT(KEYD_LEFTSHIFT);
	TAP(A);
	EXPECT(-KEYD_LEFTSHIFT, KEYD_MINUS, -KEYD_MINUS, KEYD_LEFTSHIFT);
	UP(F5);
	EXPECT(-KEYD_LEFTSHIFT);

	reset();
	DOWN(F9); TAP(A); UP(F9);
	EXPECT(KEYD_LEFTSHIFT, KEYD_A, -KEYD_A, -KEYD_LEFTSHIFT);
}

static void test_explicit_actions_and_chords(void)
{
	const char *actions[] = {
		"number.a = layer(control)",
		"number.a = timeout(x, 20, layer(control))",
	};
	for (size_t i = 0; i < ARRAY_SIZE(actions); i++) {
		reset();
		assert(kbd_eval(kbd, actions[i]) == 0);
		DOWN(F2); DOWN(A);
		WAIT(21);
		EXPECT(KEYD_LEFTCTRL);
		assert(!active("meta"));
		UP(A); UP(F2);
		EXPECT(-KEYD_LEFTCTRL);
	}

	reset();
	assert(kbd_eval(kbd, "main.a+s = x") == 0);
	DOWN(A); DOWN(S); UP(S); UP(A);
	EXPECT(KEYD_X, -KEYD_X);
	assert(!active("meta"));
	assert(!active("alt"));
}

static void test_noinherit(void)
{
	reset();
	DOWN(F10); DOWN(A); WAIT(201); UP(F10); UP(A);
	EXPECT();
	assert(!active("meta"));
	assert(!kbd->pending_overload.code);

	const char *plain[] = {
		"number.a = noinherit(minus)",
		"number.a = noinherit(noinherit(minus))",
	};
	for (size_t i = 0; i < ARRAY_SIZE(plain); i++) {
		reset();
		assert(kbd_eval(kbd, plain[i]) == 0);
		DOWN(F2); DOWN(A);
		EXPECT(KEYD_MINUS);
		WAIT(201);
		EXPECT();
		assert(!active("meta"));
		assert(kbd->keystate[KEYD_MINUS]);
		UP(F2); UP(A);
		EXPECT(-KEYD_MINUS);
	}

	reset();
	assert(kbd_eval(kbd, "number.a = noinherit(S-minus)") == 0);
	DOWN(F2); TAP(A); UP(F2);
	EXPECT(KEYD_LEFTSHIFT, KEYD_MINUS, -KEYD_MINUS, -KEYD_LEFTSHIFT);

	reset();
	assert(kbd_eval(kbd, "number.a = noinherit(layer(control))") == 0);
	DOWN(F2); DOWN(A);
	EXPECT(KEYD_LEFTCTRL);
	UP(A); UP(F2);
	EXPECT(-KEYD_LEFTCTRL);

	reset();
	assert(kbd_eval(kbd, "number.a = overload(control, noinherit(macro(y)))") == 0);
	DOWN(F2); TAP(A); UP(F2);
	EXPECT(KEYD_LEFTCTRL, -KEYD_LEFTCTRL, KEYD_Y, -KEYD_Y);
	assert(!kbd->active_macro);

	reset();
	assert(kbd_eval(kbd, "main.a = noinherit(a)") == 0);
	TAP(F1); DOWN(A);
	EXPECT(KEYD_LEFTSHIFT, KEYD_A, -KEYD_A, -KEYD_LEFTSHIFT);
	WAIT(201);
	EXPECT();
	assert(!active("meta"));
	UP(A);
	assert(active("magic-caps"));

	reset();
	kbd->config.has_inherit = 0;
	assert(kbd_eval(kbd, "main.a = noinherit(minus)") == 0);
	TAP(A);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);

	assert(kbd_eval(kbd, "main.a = noinherit()") < 0);
	assert(kbd_eval(kbd, "main.a = noinherit(a, b)") < 0);
	assert(kbd_eval(kbd, "main.a = noinherit(inherit)") < 0);
}

static void test_magic_caps(void)
{
	reset();
	TAP(F1);
	TAP(A);
	EXPECT(KEYD_LEFTSHIFT, KEYD_A, -KEYD_A, -KEYD_LEFTSHIFT);
	assert(active("magic-caps"));
	DOWN(F2); TAP(A);
	EXPECT(KEYD_1, -KEYD_1);
	TAP(S);
	EXPECT(KEYD_LEFTSHIFT, KEYD_MINUS, -KEYD_MINUS, -KEYD_LEFTSHIFT);
	assert(active("magic-caps"));
	UP(F2);

	WAIT(151);
	int depth = kbd->layer_state[config_get_layer_index(&kbd->config, "magic-caps")].oneshot_depth;
	DOWN(A); WAIT(201);
	EXPECT(KEYD_LEFTMETA);
	assert(kbd->layer_state[config_get_layer_index(&kbd->config, "magic-caps")].oneshot_depth == depth);
	UP(A);
	EXPECT(-KEYD_LEFTMETA);

	TAP(B);
	EXPECT(KEYD_B, -KEYD_B);
	assert(!active("magic-caps"));

	reset();
	TAP(F6); TAP(A);
	EXPECT(KEYD_LEFTSHIFT, KEYD_A, -KEYD_A, -KEYD_LEFTSHIFT);
	assert(active("legacy-caps"));
	DOWN(S);
	EXPECT(KEYD_LEFTSHIFT, KEYD_S, -KEYD_S, -KEYD_LEFTSHIFT);
	assert(!kbd->pending_overload.code);
	UP(S);
	assert(active("legacy-caps"));
	TAP(B);
	EXPECT(KEYD_B, -KEYD_B);
	assert(!active("legacy-caps"));

	reset();
	TAP(F6);
	DOWN(A); WAIT(201);
	EXPECT(KEYD_LEFTMETA);
	UP(A);
	EXPECT(-KEYD_LEFTMETA);
	assert(active("legacy-caps"));
}

static void test_composed_timeouts(void)
{
	reset();
	assert(kbd_eval(kbd, "inherit a = timeout(inherit, 200, layer(meta))") == 0);
	DOWN(A); DOWN(B); UP(B); UP(A);
	EXPECT(KEYD_A, KEYD_B, -KEYD_B, -KEYD_A);
	WAIT(151);
	DOWN(A); WAIT(201);
	EXPECT(KEYD_LEFTMETA);
	UP(A);
	EXPECT(-KEYD_LEFTMETA);

	reset();
	assert(kbd_eval(kbd, "inherit a = overloadt2(meta, timeout(inherit, 20, layer(control)), 200)") == 0);
	DOWN(F2);
	TAP(A);
	EXPECT();
	assert(kbd->pending_timeout.spontaneous);
	UP(F2);
	EXPECT(KEYD_1, -KEYD_1);
	DOWN(B); UP(B);
	EXPECT(KEYD_B, -KEYD_B);

	reset();
	assert(kbd_eval(kbd, "inherit a = overload(control, inherit)") == 0);
	TAP(A);
	EXPECT(KEYD_LEFTCTRL, -KEYD_LEFTCTRL, KEYD_A, -KEYD_A);

	reset();
	assert(kbd_eval(kbd, "inherit a = overloadt(meta, inherit, 200)") == 0);
	TAP(A);
	EXPECT(KEYD_A, -KEYD_A);
}

static void test_runtime_and_parser(void)
{
	const char *invalid[] = {
		"inherit a = lettermod(meta, a, 150, 200)",
		"inherit a = overloadi(inherit, inherit, 150)",
		"inherit a = overloadt2(missing, inherit, 200)",
		"inherit a = overloadt2(inherit, inherit, 200)",
		"inherit a = oneshotk(meta, inherit)",
		"inherit a = timeout(inherit, 100, swap(control))",
		"inherit a = macro(inherit)",
		"inherit a = command(inherit)",
		"inherit a = noinherit(inherit)",
		"inherit a = overloadt2(meta, macro(inherit), 200)",
		"inherit a = inherit",
		"inherit a =",
		"inherit a",
		"inherit C-a = lettermod(meta, inherit, 150, 200)",
		"inherit unknown-alias = lettermod(meta, inherit, 150, 200)",
		"inherit = lettermod(meta, inherit, 150, 200)",
		"inherit a+s = lettermod(meta, inherit, 150, 200)",
		"number.inherit a = lettermod(meta, inherit, 150, 200)",
		"number+nav.inherit a = lettermod(meta, inherit, 150, 200)",
		"a = inherit",
	};

	reset();
	for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
		size_t descriptors = kbd->config.nr_descriptors;
		size_t macros = kbd->config.nr_macros;
		assert(kbd_eval(kbd, invalid[i]) < 0);
		assert(kbd->config.nr_descriptors == descriptors);
		assert(kbd->config.nr_macros == macros);
	}
	TAP(A);
	EXPECT(KEYD_A, -KEYD_A);
	assert(kbd_eval(kbd, "main.inherit a = lettermod(control, inherit, 150, 200)") == 0);
	WAIT(151);
	DOWN(A); WAIT(201);
	EXPECT(KEYD_LEFTCTRL);
	UP(A);
	EXPECT(-KEYD_LEFTCTRL);
	assert(kbd_eval(kbd, "main.a = minus") == 0);
	TAP(A);
	EXPECT(KEYD_MINUS, -KEYD_MINUS);
	assert(kbd_eval(kbd, "reset") == 0);
	TAP(A);
	EXPECT(KEYD_A, -KEYD_A);

	const char *dots[] = {
		"inherit . = lettermod(meta, inherit, 150, 200)",
		"main.inherit . = lettermod(meta, inherit, 150, 200)",
		"inherit\t. = lettermod(meta, inherit, 150, 200)",
	};
	for (size_t i = 0; i < ARRAY_SIZE(dots); i++) {
		reset();
		assert(kbd_eval(kbd, dots[i]) == 0);
		TAP(DOT);
		EXPECT(KEYD_DOT, -KEYD_DOT);
	}

	reset();
	kbd->config.nr_descriptors = ARRAY_SIZE(kbd->config.descriptors);
	assert(kbd_eval(kbd, "inherit a = lettermod(control, inherit, 150, 200)") < 0);
	assert(kbd->config.inherit_bindings[KEYD_A].op == OP_OVERLOAD_IDLE_TIMEOUT);
}

int main(int argc, char **argv)
{
	if (argc != 2 || config_parse(&config, argv[1]))
		return 1;
	test_tap_hold_and_repeat();
	test_layers_and_noop();
	test_overrides_and_modifiers();
	test_explicit_actions_and_chords();
	test_noinherit();
	test_magic_caps();
	test_composed_timeouts();
	test_runtime_and_parser();
	free(kbd);
	puts("Inherited binding tests PASSED");
	return 0;
}
