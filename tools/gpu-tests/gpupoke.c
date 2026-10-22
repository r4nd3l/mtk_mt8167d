// Write one 64-bit GPU CR via /dev/mem (lo word then hi word). usage: gpupoke 0x240 0x8000170000
// Only for known registers; GPU must be powered (power/control = on).
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
int main(int argc, char **argv)
{
	if (argc != 3) return 2;
	uint32_t off = strtoul(argv[1], NULL, 0);
	uint64_t v = strtoull(argv[2], NULL, 0);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *b = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x13000000);
	if (fd < 0 || b == MAP_FAILED || off >= 0x10000) { perror("map"); return 1; }
	b[off / 4] = (uint32_t)v;
	b[off / 4 + 1] = (uint32_t)(v >> 32);
	printf("0x%04x <- %016llx, readback %08x%08x\n", off, (unsigned long long)v, b[off / 4 + 1], b[off / 4]);
	return 0;
}
