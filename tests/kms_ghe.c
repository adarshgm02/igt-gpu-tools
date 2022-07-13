/*
 * Copyright © 2022 Intel Corporation
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
 * Author:
 *  G M, Adarsh<adarsh.g.m@intel.com>
 */

#include "igt.h"
#include "igt_vec.h"
#include "DisplayPc.h"

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define GHE_ENABLE		1
#define GHE_DISABLE		0

#define FILENAME1 "ghe1.jpg"
#define FILENAME2 "ghe2.png"



static void prepare_pipe(igt_display_t *display, enum pipe pipe,
		igt_output_t *output, struct igt_fb *fb)
{
	drmModeModeInfo *mode = igt_output_get_mode(output);
	igt_create_image_fb(display->drm_fd, mode->hdisplay, 
			mode->vdisplay, DRM_FORMAT_XRGB8888, 
			DRM_FORMAT_MOD_LINEAR, FILENAME1, fb);
	igt_output_set_pipe(output, pipe);
	igt_plane_set_fb(igt_output_get_plane_type(output, 
				DRM_PLANE_TYPE_PRIMARY), fb);
	igt_display_commit2(display, 
			display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void cleanup_pipe(igt_display_t *display, enum pipe pipe,
		igt_output_t *output, struct igt_fb *fb)
{
	igt_plane_t *plane;
	for_each_plane_on_pipe(display, pipe, plane)
		igt_plane_set_fb(plane, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display,
			display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	igt_remove_fb(display->drm_fd, fb);
}

static void ghe_uevent(int timeout)
{
	struct udev_monitor *uevent_monitor;
	
	uevent_monitor = igt_watch_uevents();
	igt_flush_uevents(uevent_monitor);
	igt_assert(igt_ghe_histogram_event_detected(uevent_monitor, timeout));
	igt_cleanup_uevents(uevent_monitor);
}

static drmModePropertyBlobRes *get_ghe_blob(int fd, uint32_t type,
		uint32_t id, const char *name)
{
	drmModePropertyBlobRes *blob = NULL;
	uint64_t blob_id;
	int ret;

	ret = kmstest_get_property(fd,
			id,
			type,
			name,
			NULL, &blob_id, NULL);
	if (ret)
		blob = drmModeGetPropertyBlob(fd, blob_id);

	igt_assert(blob);
	igt_debug("Successfully read the Global Histogram Blob Property\n");
	return blob;
}

static int set_pixel_factor(igt_pipe_t *pipe,
		GlobalHist_ARGS *argsPtr)
{
	size_t size;
	uint32_t DietFactor[GlobalHist_IET_LUT_LENGTH];

	memcpy(DietFactor, argsPtr->DietFactor, sizeof(argsPtr->DietFactor));

	size = sizeof(DietFactor);

	for (int i = 0; i < GlobalHist_IET_LUT_LENGTH; i++) {
		/*Displaying IET LUT */
		igt_info("Pixel Factor[%d] = %d\n", i, DietFactor[i]);
	}
	igt_pipe_obj_replace_prop_blob(pipe, IGT_CRTC_GHE_PIXEL_FACTOR,
			DietFactor, size);
	return 0;
}

static GlobalHist_ARGS *send_data_to_ghe_algorithm(igt_display_t *display,
		enum pipe pipe, igt_output_t *output)
{
	drmModePropertyBlobRes *ghe_blob;
	GlobalHist_ARGS *argsPtr =
		(GlobalHist_ARGS *)malloc(sizeof(GlobalHist_ARGS));
	uint32_t Histogram[GlobalHist_BIN_COUNT], *Histogram_ptr;
	drmModeModeInfo *mode;

	mode = igt_output_get_mode(output);
	igt_info("Waiting for GHE Uevent\n");
	ghe_uevent(25);
	/*Delay For Histogram Collection*/
	sleep(2);

	ghe_blob = get_ghe_blob(display->drm_fd, DRM_MODE_OBJECT_CRTC,
			display->pipes[pipe].crtc_id, "Global Histogram");

	igt_assert_f(ghe_blob, "Failed to read GHE HIstogram Blob\n");

	Histogram_ptr	= (uint32_t *) ghe_blob->data;
	for (int i = 0; i < GlobalHist_BIN_COUNT ; i++) {
		Histogram[i] = *(Histogram_ptr + i);
		igt_info("Historgram[%d] = %d\n", i, Histogram[i]);
	}

	memcpy(argsPtr->Histogram, Histogram, sizeof(Histogram));
	argsPtr->Resolution_X = mode->hdisplay;
	argsPtr->Resolution_Y = mode->vdisplay;

	igt_debug("Making the call to GHE Alogorithm Library");

	SetHistogramDataBin(argsPtr);

	drmModeFreePropertyBlob(ghe_blob);

	return argsPtr;
}

static void enable_ghe_property(int fd, uint32_t type, uint32_t id)
{
	drmModeObjectPropertiesPtr props_ghe, props =
		drmModeObjectGetProperties(fd, id, type);
	int i, ret;
	uint32_t ghe_id;
	drmModeAtomicReqPtr req = NULL;
	
	igt_assert(props);
	req = drmModeAtomicAlloc();
	for (i = 0; i < props->count_props; i++) {
		uint32_t prop_id = props->props[i];
		uint64_t prop_value = props->prop_values[i];
		drmModePropertyPtr prop = drmModeGetProperty(fd, prop_id);
		
		igt_assert(prop);
		if (strcmp(prop->name, "GLOBAL_HIST_EN"))
			continue;
		igt_debug("prop_id=%d ,property value=%ld,name =%s\n", 
				prop_id, prop_value, prop->name);
		ghe_id = prop_id;
		igt_info("Setting GHE ENUM Property to Enable\n");

		ret = drmModeAtomicAddProperty(req, id, ghe_id, GHE_ENABLE);
		igt_assert(ret >= 0);
		ret = drmModeAtomicCommit(fd, req,
				DRM_MODE_ATOMIC_NONBLOCK, NULL);
		igt_assert_eq(ret, 0);

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
	props_ghe = drmModeObjectGetProperties(fd, id, type);
	for (i = 0; i < props_ghe->count_props; i++) {
		uint32_t prop_id_2 = props_ghe->props[i];
		uint64_t prop_value_2 = props_ghe->prop_values[i];
		drmModePropertyPtr prop_ghe =
			drmModeGetProperty(fd, prop_id_2);

		igt_assert(prop_ghe);
		if (strcmp(prop_ghe->name, "GLOBAL_HIST_EN"))
			continue;
		igt_debug("Values After Enabling GHE : "
			       "prop_id=%d, property value=%ld, name =%s\n",
				prop_id_2, prop_value_2, prop_ghe->name);

		igt_assert_f(prop_value_2 == GHE_ENABLE,
				"GHE ENABLE FAILED\n");
		drmModeFreeProperty(prop_ghe);
	}

	drmModeFreeObjectProperties(props_ghe);

	ret = drmModeAtomicCommit(fd, req, 0, NULL);
	igt_assert_eq(ret, 0);
	drmModeAtomicFree(req);
}

static void run_ghe_pipeline(igt_display_t *display,
		enum pipe pipe, igt_output_t *output)
{
	struct igt_fb fb;
	GlobalHist_ARGS *args;

	prepare_pipe(display, pipe, output, &fb);
	igt_info("Enabling GHE on %s (output: %s).\n",
			kmstest_pipe_name(pipe), output->name);
	enable_ghe_property(display->drm_fd,
			DRM_MODE_OBJECT_CRTC, display->pipes[pipe].crtc_id);

	cleanup_pipe(display, pipe, output, &fb);
	prepare_pipe(display, pipe, output, &fb);

	igt_info("Reading the Histogram Blob on %s (output: %s) "
			"and Passing it to the GHE Library.\n",
			kmstest_pipe_name(pipe), output->name);
	args = send_data_to_ghe_algorithm(display, pipe, output);

	igt_info("Writing Pixel Factor Blob\n");
	igt_assert_eq(set_pixel_factor(display->pipes, args), 0);
	igt_display_commit2(display,
			display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	cleanup_pipe(display, pipe, output, &fb);
}

static void
run_tests_for_ghe(igt_display_t *display)
{
	bool found_any_valid_pipe = false;
	enum pipe pipe;
	drmModeConnectorPtr con;
	igt_output_t *output;

	pipe = PIPE_A;
	igt_skip_on(!display->is_atomic);
	for_each_valid_output_on_pipe(display, pipe, output) {
		found_any_valid_pipe = true;
		con = output->config.connector;
		if (con->connector_type == DRM_MODE_CONNECTOR_eDP)
			run_ghe_pipeline(display, pipe, output);
		break;
	}

	igt_skip_on(!found_any_valid_pipe);
}

igt_main
{
	igt_display_t display;
	
	igt_fixture {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&display, display.drm_fd);
	}
	igt_describe("Verifyng GHE Enablement - Read Histogram "
		     "Blob - Write Pixel factor blob");

	igt_subtest("Enable-GHE")
		run_tests_for_ghe(&display);

	igt_fixture {
		igt_display_fini(&display);

	}
}
