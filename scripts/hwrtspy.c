// LD_PRELOAD ioctl spy: prints the PowerVR HWRT dataset creation arguments.
// gcc -shared -fPIC -o hwrtspy.so hwrtspy.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
struct geom { uint64_t tpc; uint32_t tpc_size, tpc_stride; uint64_t vheap, rtc; };
struct rtd { uint64_t mlist, mta, rgn; };
struct hwrt { struct geom g; struct rtd rt[2]; uint32_t fl[2], w, h, samples, layers,
	mlx, mly, msx, msy, mux, muy, rgn_size, handle; };
int ioctl(int fd, unsigned long req, ...)
{
	static int (*real)(int, unsigned long, void *);
	va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
	if (!real) real = dlsym(RTLD_NEXT, "ioctl");
	if ((req & 0xff) == 0x40 + 0x0b && (req >> 8 & 0xff) == 'd') {
		struct hwrt *a = arg;
		const char *m = getenv("RGN_MUL");
		if (m) a->rgn_size *= atoi(m);
		fprintf(stderr, "HWRT %ux%u s%u l%u tpc=%#llx size=%u stride=%u vheap=%#llx rtc=%#llx rgn_size=%u merge lo %u,%u sc %u,%u up %u,%u\n",
			a->w, a->h, a->samples, a->layers, (unsigned long long)a->g.tpc, a->g.tpc_size, a->g.tpc_stride,
			(unsigned long long)a->g.vheap, (unsigned long long)a->g.rtc, a->rgn_size, a->mlx, a->mly, a->msx, a->msy, a->mux, a->muy);
		for (int i = 0; i < 2; i++)
			fprintf(stderr, "  rt%d mlist=%#llx mta=%#llx rgn=%#llx\n", i, (unsigned long long)a->rt[i].mlist,
				(unsigned long long)a->rt[i].mta, (unsigned long long)a->rt[i].rgn);
	}
	return real(fd, req, arg);
}
