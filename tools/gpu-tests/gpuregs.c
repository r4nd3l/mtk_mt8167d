// Dump GE8300 control registers through /dev/mem (needs !STRICT_DEVMEM and the
// GPU runtime-resumed: echo on > /sys/bus/platform/devices/13000000.gpu/power/control).
// usage: gpuregs [loops]  -> prints every distinct nonzero value seen per register
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static const uint32_t regs[] = { 0x0000, 0x3800, 0x3828, 0x240, 0x248, 0x2E0, 0xFA0, 0x11D0, 0x11D8, 0xC00, 0xC08, 0xC10, 0xC18, 0xC20, 0xC28, 0xC30, 0xC38, 0xC40, 0xC48, 0xC50, 0xC58,
	0xC60, 0xC68, 0xC70, 0xC78, 0xC80, 0xC88, 0xC90, 0xC98, 0xCA0, 0xCA8,
	0xF00, 0xF08, 0xF10, 0xF18, 0xF20, 0xF28, 0xF30, 0xF38, 0xF40, 0xF48 };
#define N (sizeof(regs) / sizeof(regs[0]))
#define MAXV 8

int main(int argc, char **argv)
{
	long loops = argc > 1 ? atol(argv[1]) : 2000000;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	volatile uint8_t *b = mmap(NULL, 0x10000, PROT_READ, MAP_SHARED, fd, 0x13000000);
	if (b == MAP_FAILED) { perror("mmap"); return 1; }
	uint64_t seen[N][MAXV] = { 0 };
	int ns[N] = { 0 };
	for (long l = 0; l < loops; l++)
		for (unsigned i = 0; i < N; i++) {
			uint64_t v = *(volatile uint32_t *)(b + regs[i]) | (uint64_t)*(volatile uint32_t *)(b + regs[i] + 4) << 32;
			int k;
			for (k = 0; k < ns[i] && seen[i][k] != v; k++)
				;
			if (k == ns[i] && ns[i] < MAXV)
				seen[i][ns[i]++] = v;
		}
	for (unsigned i = 0; i < N; i++) {
		printf("0x%04x:", regs[i]);
		for (int k = 0; k < ns[i]; k++)
			printf(" %016llx", (unsigned long long)seen[i][k]);
		putchar('\n');
	}
	return 0;
}
