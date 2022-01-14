/*
 * Copyright © 2014 Intel Corporation
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
 *    Mika Kuoppala <mika.kuoppala@intel.com>
 *    Oscar Mateo <oscar.mateo@intel.com>
 *
 */

#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_store.h"
#include "igt_sysfs.h"
#include "igt_debugfs.h"
#include "sw_sync.h"

#ifndef I915_PARAM_CMD_PARSER_VERSION
#define I915_PARAM_CMD_PARSER_VERSION       28
#endif

static int device = -1;
static int sysfs = -1;

IGT_TEST_DESCRIPTION("Tests for hang detection and recovery");

static bool has_error_state(int dir)
{
	bool result;
	int fd;

	fd = openat(dir, "error", O_RDONLY);
	if (fd < 0)
		return false;

	if (read(fd, &result, sizeof(result)) < 0)
		result = false;
	else
		result = true;

	close(fd);
	return result;
}

static void assert_entry(const char *s, bool expect)
{
	char *error;

	error = igt_sysfs_get(sysfs, "error");
	igt_assert(error);

	igt_assert_f(!!strcasecmp(error, s) != expect,
		     "contents of error: '%s' (expected %s '%s')\n",
		     error, expect ? "": "not", s);

	free(error);
}

static void assert_error_state_clear(void)
{
	assert_entry("no error state collected", true);
}

static void assert_error_state_collected(void)
{
	assert_entry("no error state collected", false);
}

static void clear_error_state(void)
{
	igt_sysfs_write(sysfs, "error", "", 1);
}

static void test_error_state_basic(void)
{
	int fd;

	clear_error_state();
	assert_error_state_clear();

	/* Manually trigger a hang by request a reset */
	fd = igt_debugfs_open(device, "i915_wedged", O_WRONLY);
	igt_ignore_warn(write(fd, "1\n", 2));
	close(fd);

	assert_error_state_collected();

	clear_error_state();
	assert_error_state_clear();
}

static FILE *open_error(void)
{
	int fd;

	fd = openat(sysfs, "error", O_RDONLY);
	if (fd < 0)
		return NULL;

	return fdopen(fd, "r");
}

static bool uses_cmd_parser(void)
{
	int parser_version = 0;
	drm_i915_getparam_t gp;

	gp.param = I915_PARAM_CMD_PARSER_VERSION;
	gp.value = &parser_version;
	drmIoctl(device, DRM_IOCTL_I915_GETPARAM, &gp);

	return parser_version > 0;
}

static void check_error_state(const char *expected_ring_name,
			      uint64_t expected_offset,
			      const uint32_t *batch)
{
	bool cmd_parser = uses_cmd_parser();
	FILE *file = open_error();
	char *line = NULL;
	size_t line_size = 0;
	bool found = false;

	igt_assert(getline(&line, &line_size, file) != -1);
	igt_require(strcasecmp(line, "No error state collected"));

	igt_debug("%s(expected ring name=%s, expected offset=%"PRIx64")\n",
		  __func__, expected_ring_name, expected_offset);

	while (getline(&line, &line_size, file) > 0) {
		char *dashes;
		uint32_t gtt_offset_upper, gtt_offset_lower;
		int matched;

		dashes = strstr(line, "---");
		if (!dashes)
			continue;

		matched = sscanf(dashes, "--- gtt_offset = 0x%08x %08x\n",
				 &gtt_offset_upper, &gtt_offset_lower);
		if (!matched)
			matched = sscanf(dashes, "--- batch = 0x%08x %08x\n",
					 &gtt_offset_upper, &gtt_offset_lower);
		if (matched) {
			char expected_line[128];
			uint64_t gtt_offset;
			int i;

			strncpy(expected_line, line, dashes - line);
			expected_line[dashes - line - 1] = '\0';
			igt_assert(strstr(expected_line, expected_ring_name));

			gtt_offset = gtt_offset_upper;
			if (matched == 2) {
				gtt_offset <<= 32;
				gtt_offset |= gtt_offset_lower;
			}
			if (!cmd_parser)
				igt_assert_eq_u64(gtt_offset, expected_offset);

			for (i = 0; i < 1024; i++) {
				igt_assert(getline(&line, &line_size, file) > 0);
				if (line[0] == ':' || line[0] == '~')
					break;

				snprintf(expected_line, sizeof(expected_line),
					 "%08x :  %08x",
					 4*i, batch[i]);
				igt_assert(strstr(line, expected_line));
			}

			found = true;
			break;
		}
	}

	free(line);
	fclose(file);

	clear_error_state();

	igt_assert(found);
}

static void test_error_state_capture(const intel_ctx_t *ctx,
				     const struct intel_execution_engine2 *e)
{
	uint32_t *batch;
	igt_hang_t hang;
	uint64_t offset;
	uint64_t ahnd = get_reloc_ahnd(device, ctx->id);

	clear_error_state();

	hang = igt_hang_ctx_with_ahnd(device, ahnd, ctx->id, e->flags,
				      HANG_ALLOW_CAPTURE);
	offset = hang.spin->obj[IGT_SPIN_BATCH].offset;

	batch = gem_mmap__cpu(device, hang.spin->handle, 0, 4096, PROT_READ);
	gem_set_domain(device, hang.spin->handle, I915_GEM_DOMAIN_CPU, 0);

	igt_post_hang_ring(device, hang);

	check_error_state(e->name, offset, batch);
	munmap(batch, 4096);
	put_ahnd(ahnd);
}

static void
test_engine_hang(const intel_ctx_t *ctx,
		 const struct intel_execution_engine2 *e, unsigned int flags)
{
	const struct intel_execution_engine2 *other;
	const intel_ctx_t *tmp_ctx;
	igt_spin_t *spin, *next;
	IGT_LIST_HEAD(list);
	uint64_t ahnd = get_reloc_ahnd(device, ctx->id), ahndN;

	igt_skip_on(flags & IGT_SPIN_INVALID_CS &&
		    gem_engine_has_cmdparser(device, &ctx->cfg, e->flags));

	/* Fill all the other engines with background load */
	for_each_ctx_engine(device, ctx, other) {
		if (other->flags == e->flags)
			continue;

		tmp_ctx = intel_ctx_create(device, &ctx->cfg);
		ahndN = get_reloc_ahnd(device, tmp_ctx->id);
		spin = __igt_spin_new(device,
				      .ahnd = ahndN,
				      .ctx = tmp_ctx,
				      .engine = other->flags,
				      .flags = IGT_SPIN_FENCE_OUT);
		intel_ctx_destroy(device, tmp_ctx);

		igt_list_move(&spin->link, &list);
	}

	/* And on the target engine, we hang */
	spin = igt_spin_new(device,
			    .ahnd = ahnd,
			    .ctx = ctx,
			    .engine = e->flags,
			    .flags = (IGT_SPIN_FENCE_OUT |
				      IGT_SPIN_NO_PREEMPTION |
				      flags));

	/* Wait for the hangcheck to terminate the hanger */
	igt_assert(sync_fence_wait(spin->out_fence, 30000) == 0); /* 30s */
	igt_assert_eq(sync_fence_status(spin->out_fence), -EIO);
	igt_spin_free(device, spin);

	/* But no other engines/clients should be affected */
	igt_list_for_each_entry_safe(spin, next, &list, link) {
		ahndN = spin->ahnd;
		igt_assert(sync_fence_wait(spin->out_fence, 0) == -ETIME);
		igt_spin_end(spin);

		igt_assert(sync_fence_wait(spin->out_fence, 500) == 0);
		igt_assert_eq(sync_fence_status(spin->out_fence), 1);
		igt_spin_free(device, spin);
		put_ahnd(ahndN);
	}
	put_ahnd(ahnd);
}

static int hang_count;

static void sig_io(int sig)
{
	hang_count++;
}

static void test_hang_detector(const intel_ctx_t *ctx,
			       const struct intel_execution_engine2 *e)
{
	igt_hang_t hang;
	uint64_t ahnd = get_reloc_ahnd(device, ctx->id);

	hang_count = 0;

	igt_fork_hang_detector(device);

	/* Steal the signal handler */
	signal(SIGIO, sig_io);

	/* Make a hang... */
	hang = igt_hang_ctx_with_ahnd(device, ahnd, ctx->id, e->flags, 0);

	igt_post_hang_ring(device, hang);
	put_ahnd(ahnd);

	igt_stop_hang_detector();

	/* Did it work? */
	igt_assert(hang_count == 1);
}

/* This test covers the case where we end up in an uninitialised area of the
 * ppgtt and keep executing through it. This is particularly relevant if 48b
 * ppgtt is enabled because the ppgtt is massively bigger compared to the 32b
 * case and it takes a lot more time to wrap, so the acthd can potentially keep
 * increasing for a long time
 */
static void hangcheck_unterminated(const intel_ctx_t *ctx)
{
	/* timeout needs to be greater than ~5*hangcheck */
	int64_t timeout_ns = 100ull * NSEC_PER_SEC; /* 100 seconds */
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 gem_exec;
	uint32_t handle;

	igt_require(gem_uses_full_ppgtt(device));
	igt_require_hang_ring(device, ctx->id, 0);

	handle = gem_create(device, 4096);

	memset(&gem_exec, 0, sizeof(gem_exec));
	gem_exec.handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)&gem_exec;
	execbuf.buffer_count = 1;
	execbuf.rsvd1 = ctx->id;

	gem_execbuf(device, &execbuf);
	if (gem_wait(device, handle, &timeout_ns) != 0) {
		/* need to manually trigger a hang to clean before failing */
		igt_force_gpu_reset(device);
		igt_assert_f(0, "unterminated batch did not trigger a hang!\n");
	}
}

static void do_tests(const char *name, const char *prefix,
		     const intel_ctx_t *ctx)
{
	const struct intel_execution_engine2 *e;
	char buff[256];

	snprintf(buff, sizeof(buff), "Per engine error capture (%s reset)", name);
	igt_describe(buff);
	snprintf(buff, sizeof(buff), "%s-error-state-capture", prefix);
	igt_subtest_with_dynamic(buff) {
		for_each_ctx_engine(device, ctx, e) {
			igt_dynamic_f("%s", e->name)
				test_error_state_capture(ctx, e);
		}
	}

	snprintf(buff, sizeof(buff), "Per engine hang recovery (spin, %s reset)", name);
	igt_describe(buff);
	snprintf(buff, sizeof(buff), "%s-engine-hang", prefix);
	igt_subtest_with_dynamic(buff) {
                int has_gpu_reset = 0;
		struct drm_i915_getparam gp = {
			.param = I915_PARAM_HAS_GPU_RESET,
			.value = &has_gpu_reset,
		};

		igt_require(gem_scheduler_has_preemption(device));
		igt_params_set(device, "reset", "%u", -1);
                ioctl(device, DRM_IOCTL_I915_GETPARAM, &gp);
		igt_require(has_gpu_reset > 1);

		for_each_ctx_engine(device, ctx, e) {
			igt_dynamic_f("%s", e->name)
				test_engine_hang(ctx, e, 0);
		}
	}

	snprintf(buff, sizeof(buff), "Per engine hang recovery (invalid CS, %s reset)", name);
	igt_describe(buff);
	snprintf(buff, sizeof(buff), "%s-engine-error", prefix);
	igt_subtest_with_dynamic(buff) {
		int has_gpu_reset = 0;
		struct drm_i915_getparam gp = {
			.param = I915_PARAM_HAS_GPU_RESET,
			.value = &has_gpu_reset,
		};

		igt_params_set(device, "reset", "%u", -1);
		ioctl(device, DRM_IOCTL_I915_GETPARAM, &gp);
		igt_require(has_gpu_reset > 1);

		for_each_ctx_engine(device, ctx, e) {
			igt_dynamic_f("%s", e->name)
				test_engine_hang(ctx, e, IGT_SPIN_INVALID_CS);
		}
	}
}

igt_main
{
	const intel_ctx_t *ctx;
	igt_hang_t hang = {};

	igt_fixture {
		device = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(device);

		ctx = intel_ctx_create_all_physical(device);

		hang = igt_allow_hang(device, ctx->id, HANG_ALLOW_CAPTURE);

		sysfs = igt_sysfs_open(device);
		igt_assert(sysfs != -1);

		igt_require(has_error_state(sysfs));
	}

	igt_describe("Basic error capture");
	igt_subtest("error-state-basic")
		test_error_state_basic();

	igt_describe("Check that executing unintialised memory causes a hang");
	igt_subtest("hangcheck-unterminated")
		hangcheck_unterminated(ctx);

	igt_describe("Check that hang detector works");
	igt_subtest_with_dynamic("detector") {
		const struct intel_execution_engine2 *e;

		for_each_ctx_engine(device, ctx, e) {
			igt_dynamic_f("%s", e->name)
				test_hang_detector(ctx, e);
		}
	}

	do_tests("GT", "gt", ctx);

	igt_fixture {
		igt_disallow_hang(device, hang);

		hang = igt_allow_hang(device, ctx->id, HANG_ALLOW_CAPTURE | HANG_WANT_ENGINE_RESET);
	}

	do_tests("engine", "engine", ctx);

	igt_fixture {
		igt_disallow_hang(device, hang);
		intel_ctx_destroy(device, ctx);
		close(device);
	}
}
