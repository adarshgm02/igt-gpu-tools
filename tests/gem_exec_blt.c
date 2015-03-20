/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include "drm.h"
#include "ioctl_wrappers.h"
#include "drmtest.h"
#include "intel_chipset.h"
#include "intel_io.h"

#define OBJECT_SIZE 16384

#define COPY_BLT_CMD		(2<<29|0x53<<22|0x6)
#define BLT_WRITE_ALPHA		(1<<21)
#define BLT_WRITE_RGB		(1<<20)
#define BLT_SRC_TILED		(1<<15)
#define BLT_DST_TILED		(1<<11)

#define LOCAL_I915_EXEC_NO_RELOC (1<<11)
#define LOCAL_I915_EXEC_HANDLE_LUT (1<<12)

static int gem_linear_blt(int fd,
			  uint32_t *batch,
			  uint32_t src,
			  uint32_t dst,
			  uint32_t length,
			  struct drm_i915_gem_relocation_entry *reloc)
{
	uint32_t *b = batch;
	int height = length / (16 * 1024);

	igt_assert_lte(height, 1 << 16);

	if (height) {
		int i = 0;
		b[i++] = COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			b[i-1]+=2;
		b[i++] = 0xcc << 16 | 1 << 25 | 1 << 24 | (16*1024);
		b[i++] = 0;
		b[i++] = height << 16 | (4*1024);
		b[i++] = 0;
		reloc->offset = (b-batch+4) * sizeof(uint32_t);
		reloc->delta = 0;
		reloc->target_handle = dst;
		reloc->read_domains = I915_GEM_DOMAIN_RENDER;
		reloc->write_domain = I915_GEM_DOMAIN_RENDER;
		reloc->presumed_offset = 0;
		reloc++;
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			b[i++] = 0; /* FIXME */

		b[i++] = 0;
		b[i++] = 16*1024;
		b[i++] = 0;
		reloc->offset = (b-batch+7) * sizeof(uint32_t);
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			reloc->offset += sizeof(uint32_t);
		reloc->delta = 0;
		reloc->target_handle = src;
		reloc->read_domains = I915_GEM_DOMAIN_RENDER;
		reloc->write_domain = 0;
		reloc->presumed_offset = 0;
		reloc++;
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			b[i++] = 0; /* FIXME */

		b += i;
		length -= height * 16*1024;
	}

	if (length) {
		int i = 0;
		b[i++] = COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			b[i-1]+=2;
		b[i++] = 0xcc << 16 | 1 << 25 | 1 << 24 | (16*1024);
		b[i++] = height << 16;
		b[i++] = (1+height) << 16 | (length / 4);
		b[i++] = 0;
		reloc->offset = (b-batch+4) * sizeof(uint32_t);
		reloc->delta = 0;
		reloc->target_handle = dst;
		reloc->read_domains = I915_GEM_DOMAIN_RENDER;
		reloc->write_domain = I915_GEM_DOMAIN_RENDER;
		reloc->presumed_offset = 0;
		reloc++;
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			b[i++] = 0; /* FIXME */

		b[i++] = height << 16;
		b[i++] = 16*1024;
		b[i++] = 0;
		reloc->offset = (b-batch+7) * sizeof(uint32_t);
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			reloc->offset += sizeof(uint32_t);
		reloc->delta = 0;
		reloc->target_handle = src;
		reloc->read_domains = I915_GEM_DOMAIN_RENDER;
		reloc->write_domain = 0;
		reloc->presumed_offset = 0;
		reloc++;
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			b[i++] = 0; /* FIXME */

		b += i;
	}

	b[0] = MI_BATCH_BUFFER_END;
	b[1] = 0;

	return (b+2 - batch) * sizeof(uint32_t);
}

static double elapsed(const struct timeval *start,
		      const struct timeval *end,
		      int loop)
{
	return (1e6*(end->tv_sec - start->tv_sec) + (end->tv_usec - start->tv_usec))/loop;
}

static const char *bytes_per_sec(char *buf, double v)
{
	const char *order[] = {
		"",
		"KiB",
		"MiB",
		"GiB",
		"TiB",
		"PiB",
		NULL,
	}, **o = order;

	while (v > 1024 && o[1]) {
		v /= 1024;
		o++;
	}
	sprintf(buf, "%.1f%s/s", v, *o);
	return buf;
}

static uint32_t dumb_create(int fd)
{
	struct drm_mode_create_dumb arg;
	int ret;

	arg.bpp = 32;
	arg.width = 32;
	arg.height = 32;

	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
	igt_assert_eq(ret, 0);
	igt_assert(arg.size >= 4096);

	return arg.handle;
}

static int dcmp(const void *A, const void *B)
{
	const double *a = A, *b = B;
	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

static void run(int object_size, bool dumb)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec[3];
	struct drm_i915_gem_relocation_entry reloc[4];
	uint32_t buf[20];
	uint32_t handle, src, dst;
	int fd, len, count;
	int ring;

	fd = drm_open_any();
	if (dumb)
		handle = dumb_create(fd);
	else
		handle = gem_create(fd, 4096);

	src = gem_create(fd, object_size);
	dst = gem_create(fd, object_size);

	len = gem_linear_blt(fd, buf, 0, 1, object_size, reloc);
	gem_write(fd, handle, 0, buf, len);

	memset(exec, 0, sizeof(exec));
	exec[0].handle = src;
	exec[1].handle = dst;

	exec[2].handle = handle;
	if (intel_gen(intel_get_drm_devid(fd)) >= 8)
		exec[2].relocation_count = len > 56 ? 4 : 2;
	else
		exec[2].relocation_count = len > 40 ? 4 : 2;
	exec[2].relocs_ptr = (uintptr_t)reloc;

	ring = 0;
	if (HAS_BLT_RING(intel_get_drm_devid(fd)))
		ring = I915_EXEC_BLT;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)exec;
	execbuf.buffer_count = 3;
	execbuf.batch_len = len;
	execbuf.flags = ring;
	execbuf.flags |= LOCAL_I915_EXEC_HANDLE_LUT;
	execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC;

	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf)) {
		len = gem_linear_blt(fd, buf, src, dst, object_size, reloc);
		igt_assert(len == execbuf.batch_len);
		gem_write(fd, handle, 0, buf, len);
		execbuf.flags = ring;
		do_ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuf);
	}
	gem_sync(fd, handle);

	for (count = 1; count <= 1<<12; count <<= 1) {
		struct timeval start, end;
		const int reps = 9;
		double t[reps], sum;
		int n;

		for (n = 0; n < reps; n++) {
			gettimeofday(&start, NULL);
			for (int loop = 0; loop < count; loop++)
				gem_execbuf(fd, &execbuf);
			gem_sync(fd, handle);
			gettimeofday(&end, NULL);
			t[n] = elapsed(&start, &end, count);
		}
		qsort(t, n, sizeof(double), dcmp);
		sum = 0;
		for (n = 2; n < reps - 2; n++)
			sum += t[n];
		sum /= reps - 4;
		igt_info("Time to blt %d bytes x %6d:	%7.3fµs, %s\n",
			 object_size, count, sum,
			 bytes_per_sec((char *)buf, object_size/sum*1e6));
		fflush(stdout);
	}
	gem_close(fd, handle);

	close(fd);
}

int main(int argc, char **argv)
{
	int i;

	igt_subtest_init(argc, argv);

	igt_skip_on_simulation();

	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			int object_size = atoi(argv[i]);
			if (object_size)
				run((object_size + 3) & -4, false);
		}
		return 0;
	}

	igt_subtest("cold")
		run(OBJECT_SIZE, false);

	igt_subtest("normal")
		run(OBJECT_SIZE, false);

	igt_subtest("dumb-buf")
		run(OBJECT_SIZE, true);

	igt_exit();
}
