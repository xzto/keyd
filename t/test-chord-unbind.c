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
	struct output out = {
		.send_key = send_key,
		.on_layer_change = on_layer_change,
	};

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
#define WAIT(ms) event(0, 0, ms)
#define EXPECT(...) do { \
	const int expected[] = { __VA_ARGS__ }; \
	expect(expected, ARRAY_SIZE(expected), __LINE__); \
} while (0)

static void expect(const int *expected, size_t n, int line)
{
	size_t i;

	if (noutput != n) {
		fprintf(stderr, "line %d: expected %zu events, got %zu\n", line, n, noutput);
		abort();
	}

	for (i = 0; i < n; i++) {
		int actual = output[i].pressed ? output[i].code : -output[i].code;
		if (actual != expected[i]) {
			fprintf(stderr, "line %d, event %zu: expected %d, got %d\n",
				line, i, expected[i], actual);
			abort();
		}
	}

	noutput = 0;
}

static void test_order_and_release(void)
{
	int reverse, crossed;

	for (reverse = 0; reverse < 2; reverse++) {
		for (crossed = 0; crossed < 2; crossed++) {
			uint8_t first = reverse ? KEYD_K : KEYD_J;
			uint8_t second = reverse ? KEYD_J : KEYD_K;
			int a = reverse ? KEYD_5 : KEYD_4;
			int b = reverse ? KEYD_4 : KEYD_5;

			reset();
			DOWN(F1);
			assert(event(first, 1, 1) == 0);
			EXPECT(a);
			assert(event(second, 1, 1) == 0);
			EXPECT(b);
			WAIT(500);
			assert(noutput == 0);
			event(crossed ? second : first, 0, 1);
			event(crossed ? first : second, 0, 1);
			UP(F1);
			EXPECT(crossed ? -b : -a, crossed ? -a : -b);
		}
	}
}

static void test_scope(void)
{
	reset();
	DOWN(J); DOWN(K); UP(K); UP(J);
	EXPECT(KEYD_ESC, -KEYD_ESC);

	DOWN(F1);
	DOWN(J); UP(J);
	EXPECT(KEYD_4, -KEYD_4);
	UP(F1);
	DOWN(K); DOWN(J); UP(J); UP(K);
	EXPECT(KEYD_ESC, -KEYD_ESC);

	/* Unbinding j+k must not disable other inherited chords. */
	DOWN(F1);
	DOWN(D); DOWN(F); UP(F); UP(D);
	UP(F1);
	EXPECT(KEYD_DELETE, -KEYD_DELETE);
}

static void test_activation_precedence(void)
{
	reset();
	DOWN(F1); DOWN(F2);
	DOWN(J); DOWN(K); UP(J); UP(K);
	UP(F2); UP(F1);
	EXPECT(KEYD_TAB, -KEYD_TAB);

	DOWN(F2); DOWN(F1);
	DOWN(J); DOWN(K); UP(J); UP(K);
	UP(F1); UP(F2);
	EXPECT(KEYD_4, KEYD_5, -KEYD_4, -KEYD_5);

	DOWN(F1); DOWN(F7);
	DOWN(J); DOWN(K); UP(K); UP(J);
	UP(F7); UP(F1);
	EXPECT(KEYD_1, KEYD_2, -KEYD_2, -KEYD_1);
}

static void test_exact_key_sets(void)
{
	reset();
	DOWN(F3);
	DOWN(A); DOWN(B); DOWN(C); UP(C); UP(B); UP(A);
	EXPECT(KEYD_BACKSPACE, -KEYD_BACKSPACE);
	UP(F3);

	/* The two-key mask must not replace or disable the three-key chord. */
	DOWN(F3);
	DOWN(A); DOWN(B); WAIT(100); UP(B); UP(A);
	UP(F3);
	EXPECT(KEYD_A, KEYD_B, -KEYD_B, -KEYD_A);

	DOWN(F8);
	DOWN(J); DOWN(K); UP(K); UP(J);
	UP(F8);
	EXPECT(KEYD_4, KEYD_5, -KEYD_5, -KEYD_4);
}

static void test_replacement(void)
{
	reset();
	DOWN(F4);
	DOWN(J); DOWN(K); UP(K); UP(J);
	UP(F4);
	EXPECT(KEYD_ESC, -KEYD_ESC);

	DOWN(F9);
	DOWN(J); DOWN(K); UP(K); UP(J);
	UP(F9);
	EXPECT(KEYD_J, KEYD_K, -KEYD_K, -KEYD_J);

	DOWN(F10);
	DOWN(J); DOWN(K); UP(K); UP(J);
	UP(F10);
	EXPECT(KEYD_TAB, -KEYD_TAB);
}

static void test_sticky_and_oneshot(void)
{
	reset();
	DOWN(F5); UP(F5);
	DOWN(J); DOWN(K); UP(J); UP(K);
	EXPECT(KEYD_4, KEYD_5, -KEYD_4, -KEYD_5);
	DOWN(F5); UP(F5);
	DOWN(J); DOWN(K); UP(J); UP(K);
	EXPECT(KEYD_ESC, -KEYD_ESC);

	DOWN(F6); UP(F6);
	DOWN(J); UP(J);
	EXPECT(KEYD_4, -KEYD_4);
	DOWN(J); DOWN(K); UP(J); UP(K);
	EXPECT(KEYD_ESC, -KEYD_ESC);
}

static void test_space_and_z(void)
{
	reset();
	DOWN(SPACE); WAIT(201);
	assert(noutput == 0);
	DOWN(J); DOWN(K); UP(K); UP(J);
	EXPECT(KEYD_DOWN, KEYD_UP, -KEYD_UP, -KEYD_DOWN);

	DOWN(Z); WAIT(201);
	DOWN(K); DOWN(J); UP(J); UP(K);
	UP(Z);
	EXPECT(KEYD_5, KEYD_4, -KEYD_4, -KEYD_5);

	DOWN(J); UP(J);
	EXPECT(KEYD_DOWN, -KEYD_DOWN);
	UP(SPACE);
	DOWN(J); DOWN(K); UP(K); UP(J);
	EXPECT(KEYD_ESC, -KEYD_ESC);

	/* A short z tap keeps numbers active after Space is released. */
	WAIT(201);
	DOWN(SPACE); WAIT(201);
	DOWN(Z); UP(Z); UP(SPACE);
	DOWN(J); DOWN(K); UP(K); UP(J);
	EXPECT(KEYD_4, KEYD_5, -KEYD_5, -KEYD_4);

	reset();
	DOWN(X); UP(X);
	DOWN(SPACE); WAIT(201); UP(SPACE);
	EXPECT(KEYD_X, -KEYD_X, KEYD_SPACE, -KEYD_SPACE);
}

static void test_parser(void)
{
	assert(config_add_entry(&config, "numbers.j = unbind") < 0);
	assert(config_add_entry(&config, "numbers.j+ = unbind") < 0);
	assert(config_add_entry(&config, "numbers.j = overload(control, unbind)") < 0);
	assert(config_add_entry(&config, "numbers.j+k = noop") == 0);
	assert(config_add_entry(&config, "numbers.k+j = unbind") == 0);

	reset();
	DOWN(F1);
	assert(DOWN(J) == 0);
	EXPECT(KEYD_4);
	UP(J); UP(F1);
	EXPECT(-KEYD_4);
}

int main(int argc, char **argv)
{
	if (argc != 2 || config_parse(&config, argv[1]))
		return 1;

	test_order_and_release();
	test_scope();
	test_activation_precedence();
	test_exact_key_sets();
	test_replacement();
	test_sticky_and_oneshot();
	test_space_and_z();
	test_parser();
	free(kbd);

	puts("Chord unbinding tests PASSED");
	return 0;
}
