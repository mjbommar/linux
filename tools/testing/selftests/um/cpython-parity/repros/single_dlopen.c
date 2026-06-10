// SPDX-License-Identifier: GPL-2.0
/* Single-thread dlopen loop. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

#define ITERATIONS 5000

int main(void)
{
	for (int i = 0; i < ITERATIONS; i++) {
		double (*fn)(double x);
		void *h;

		h = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			fprintf(stderr, "FAIL iter=%d\n", i);
			return 1;
		}

		fn = (double (*)(double))dlsym(h, "cos");
		if (!fn || fn(0.0) != 1.0) {
			fprintf(stderr, "FAIL iter=%d\n", i);
			return 1;
		}

		if (dlclose(h) != 0) {
			fprintf(stderr, "FAIL iter=%d\n", i);
			return 1;
		}
	}

	printf("REPRO: OK iters=%d\n", ITERATIONS);
	return 0;
}
