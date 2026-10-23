// SPDX-License-Identifier: GPL-2.0-only
/*
 * Helper for argblock-offset: print the in-page offset of argv[0] and the
 * end of the [stack] VMA, which the ELF loader chooses.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long end = 0;
	char line[256];
	FILE *maps = fopen("/proc/self/maps", "r");

	while (maps && fgets(line, sizeof(line), maps))
		if (strstr(line, "[stack]"))
			sscanf(line, "%*[0-9a-f]-%lx", &end);
	if (argc < 1 || !end)
		return 1;
	printf("%lx %lx\n", (unsigned long)argv[0] & (sysconf(_SC_PAGESIZE) - 1),
	       end);
	return 0;
}
