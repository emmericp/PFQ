#include <pf_q-maybe.h>

#include <linux/ip.h>
#include <stdio.h>
#include <assert.h>

void test_signed(void)
{
     long long int a = JUST('x');
     long long int b = JUST(0);
     long long int c = JUST(-1);
     long long int d = JUST(-2);
     long long int e = NOTHING;
     long long int f = JUST(0LL);
     long long int g = JUST(1LL);
     long long int h = JUST(-1LL);
     long long int i = NOTHING;

     assert(IS_JUST(a));
     assert(IS_JUST(b));
     assert(IS_JUST(c));
     assert(IS_JUST(d));
     assert(IS_NOTHING(e));

     assert(IS_JUST(f));
     assert(IS_JUST(g));
     assert(IS_JUST(h));
     assert(IS_NOTHING(i));

     assert(FROM_JUST(char,a) == 'x');
     assert(FROM_JUST(int, b) ==  0);
     assert(FROM_JUST(int, c) == -1);
     assert(FROM_JUST(int, d) == -2);
     assert(FROM_JUST(long long int, f) ==  0);
     assert(FROM_JUST(long long int, g) ==  1);
     assert(FROM_JUST(long long int, h) == -1);
}


void test_unsigned(void)
{
	long long int a_ = JUST(10U);
	long long int b_ = JUST(0U);
	long long int c_ = JUST((unsigned int)-1);
	long long int d_ = NOTHING;

	long long int f_ = JUST(0ULL);
	long long int g_ = JUST(1ULL);
	long long int h_ = JUST(-1ULL);
	long long int i_ = NOTHING;

	assert(IS_JUST(a_));
	assert(IS_JUST(b_));
	assert(IS_JUST(c_));
	assert(IS_NOTHING(d_));
	assert(IS_JUST(f_));
	assert(IS_JUST(g_));
	assert(IS_JUST(h_));
	assert(IS_NOTHING(i_));

	assert(FROM_JUST(unsigned, a_) == 10U);
	assert(FROM_JUST(unsigned, b_) == 0U);
	assert(FROM_JUST(unsigned, c_) == (unsigned)-1);
	assert(FROM_JUST(unsigned long long, f_) == 0ULL);
	assert(FROM_JUST(unsigned long long, g_) == 1ULL);
	assert(FROM_JUST(unsigned long long, h_) == -1ULL);

}

void test_header()
{
	struct iphdr h;
	h.tos = 1;
	h.ttl = 255;

	long long int tos = JUST(h.tos);
	long long int ttl = JUST(h.ttl);
	assert(IS_JUST(tos));
	assert(IS_JUST(ttl));
	assert(FROM_JUST(unsigned char, tos) == 1);
	assert(FROM_JUST(unsigned char, ttl) == 255);
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	test_signed();
	test_unsigned();
	test_header();
	printf("All test successfully passed.\n");
	return 0;
}

