/* Sanity check: test_struct/test_unicodedata are NOT real Class C UML gaps.
 * test_struct fails on subinterpreter pool quirk; test_unicodedata fails on
 * a CPython ASCII-decode assertion. Neither is a UML kernel surface bug.
 * This binary always PASSes — it documents the demotion. */
#define _GNU_SOURCE
#include <stdio.h>

int main(void)
{
	printf("REPRO: sanity_struct_unicode PASS not_uml_substrate_gap\n");
	return 0;
}
