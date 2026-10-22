// LD_PRELOAD ioctl spy: prints the PowerVR HWRT dataset creation arguments.
// gcc -shared -fPIC -o hwrtspy.so hwrtspy.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
// DUMP_VA="0x800016e000:64,0x8000171000:1280": at HWRT destroy, hex-dump those GPU VAs.
// Every BO is created CPU-mappable (unless PM_FW_PROTECT) so this works without a Mesa rebuild.
struct map { uint32_t handle; uint64_t va, off, size; } maps[4096];
static int nmaps;
static void dump_vas(int fd, int (*real)(int, unsigned long, void *))
{
	char *s = getenv("DUMP_VA"), *save, buf[512];
	if (!s) return;
	strncpy(buf, s, sizeof(buf) - 1);
	for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		uint64_t va = strtoull(t, NULL, 0), len = strchr(t, ':') ? strtoull(strchr(t, ':') + 1, NULL, 0) : 64;
		int k;
		for (k = 0; k < nmaps && !(va >= maps[k].va && va < maps[k].va + maps[k].size); k++)
			;
		if (k == nmaps) { fprintf(stderr, "DUMPVA %#llx: unmapped\n", (unsigned long long)va); continue; }
		struct { uint32_t handle, pad; uint64_t offset; } mo = { maps[k].handle, 0, 0 };
		if (real(fd, 0xc0106442, &mo)) { perror("mmap_offset"); continue; }
		uint64_t rel = va - maps[k].va + maps[k].off, base = rel & ~4095ull;
		uint8_t *p = mmap(NULL, rel - base + len, PROT_READ, MAP_SHARED, fd, mo.offset + base);
		if (p == MAP_FAILED) { perror("mmap"); continue; }
		fprintf(stderr, "DUMPVA %#llx len %llu", (unsigned long long)va, (unsigned long long)len);
		for (uint64_t i = 0; i < len; i++)
			fprintf(stderr, i % 32 ? "%02x" : "\n%02x", p[rel - base + i]);
		fputc('\n', stderr);
	}
}
struct geom { uint64_t tpc; uint32_t tpc_size, tpc_stride; uint64_t vheap, rtc; };
struct rtd { uint64_t mlist, mta, rgn; };
struct hwrt { struct geom g; struct rtd rt[2]; uint32_t fl[2], w, h, samples, layers,
	mlx, mly, msx, msy, mux, muy, rgn_size, handle; };
int ioctl(int fd, unsigned long req, ...)
{
	static int (*real)(int, unsigned long, void *);
	va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
	if (!real) real = dlsym(RTLD_NEXT, "ioctl");
	if ((req >> 8 & 0xff) == 'd' && (req & 0xff) == 0x41) { // CREATE_BO
		uint64_t *flags = (uint64_t *)((char *)arg + 16);
		if (!(*flags & 2)) *flags |= 4;
	}
	if ((req >> 8 & 0xff) == 'd' && (req & 0xff) == 0x45 && nmaps < 4096) { // VM_MAP
		uint32_t *a = arg;
		maps[nmaps++] = (struct map){ a[4], *(uint64_t *)(a + 2), *(uint64_t *)(a + 6), *(uint64_t *)(a + 8) };
	}
	if ((req >> 8 & 0xff) == 'd' && (req & 0xff) == 0x4c) // DESTROY_HWRT_DATASET
		dump_vas(fd, real);
	if ((req & 0xff) == 0x40 + 0x0b && (req >> 8 & 0xff) == 'd') {
		struct hwrt *a = arg;
		const char *m = getenv("RGN_MUL");
		if (m) a->rgn_size *= atoi(m);
		const char *tp = getenv("TPC_SET");
		if (tp) a->g.tpc_size = atoi(tp);
		fprintf(stderr, "HWRT %ux%u s%u l%u tpc=%#llx size=%u stride=%u vheap=%#llx rtc=%#llx rgn_size=%u merge lo %u,%u sc %u,%u up %u,%u\n",
			a->w, a->h, a->samples, a->layers, (unsigned long long)a->g.tpc, a->g.tpc_size, a->g.tpc_stride,
			(unsigned long long)a->g.vheap, (unsigned long long)a->g.rtc, a->rgn_size, a->mlx, a->mly, a->msx, a->msy, a->mux, a->muy);
		for (int i = 0; i < 2; i++)
			fprintf(stderr, "  rt%d mlist=%#llx mta=%#llx rgn=%#llx\n", i, (unsigned long long)a->rt[i].mlist,
				(unsigned long long)a->rt[i].mta, (unsigned long long)a->rt[i].rgn);
	}
	return real(fd, req, arg);
}
