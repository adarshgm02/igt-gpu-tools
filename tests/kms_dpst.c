#include "igt.h"
#include "igt_vec.h"
#include "DisplayPcDpst.h"

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DPST_ENABLE		1
#define DPST_DISABLE		0
#define DPST_AGGRESSIVENESS	3
#define BACKLIGHT_PATH "/sys/class/backlight/intel_backlight"

#define FILENAME1 "dpst1.jpg"
#define FILENAME2 "dpst2.png"


bool is_edp, is_battery_mode, is_pwm, is_8bpc, is_sdr;

static void prepare_pipe(igt_display_t *display, enum pipe pipe,
		igt_output_t *output, struct igt_fb *fb)
{
	drmModeModeInfo *mode = igt_output_get_mode(output);
	igt_create_image_fb(display->drm_fd,mode->hdisplay,
			mode->vdisplay,DRM_FORMAT_XRGB8888,
			DRM_FORMAT_MOD_LINEAR,FILENAME2,fb);
        igt_output_set_pipe(output, pipe);
        igt_plane_set_fb(igt_output_get_plane_type(output,
			       	DRM_PLANE_TYPE_PRIMARY), fb);
        igt_display_commit2(display, 
			display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	
	igt_create_image_fb(display->drm_fd,mode->hdisplay,
                        mode->vdisplay,DRM_FORMAT_XRGB8888,
                        DRM_FORMAT_MOD_LINEAR,FILENAME1,fb);
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

static int backlight_read(int *result, const char *fname)
{
	int fd;
	char full[PATH_MAX];
	char dst[64];
	int r, e;
	
	igt_assert(snprintf(full, PATH_MAX, "%s/%s", BACKLIGHT_PATH, fname) < PATH_MAX);
	fd = open(full, O_RDONLY);
	if (fd == -1)
		return -errno;
	r = read(fd, dst, sizeof(dst));
	e = errno;
	close(fd);
	
	if (r < 0)
		return -e;
	errno = 0;
	*result = strtol(dst, NULL, 10);
	return errno;
}

static int dpst_backlight_write(int value, const char *fname)
{
	int fd;
	char full[PATH_MAX];
	char src[64];
	int len;

	igt_assert(snprintf(full, PATH_MAX, "%s/%s",
				BACKLIGHT_PATH, fname) < PATH_MAX);
	fd = open(full, O_WRONLY);
	if (fd == -1)
		return -errno;
	len = snprintf(src, sizeof(src), "%i", value);
	len = write(fd, src, len);
	close(fd);
	if (len < 0)
		return len;

	return 0;
}

static void dpst_uevent(int timeout)
{
	struct udev_monitor *uevent_monitor;
	uevent_monitor = igt_watch_uevents();
	igt_flush_uevents(uevent_monitor);
	igt_assert(igt_dpst_histogram_event_detected(uevent_monitor, timeout));
	igt_cleanup_uevents(uevent_monitor);
}

static drmModePropertyBlobRes *get_dpst_blob(int fd, uint32_t type,
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
	igt_debug("Successfully read the DPST Blob Property\n");
	return blob;
}

static int set_pixel_factor_and_brightness(igt_pipe_t *pipe,
		DD_DPST_ARGS *argsPtr)
{
	size_t size;
	int result;
	uint32_t DietFactor[DPST_IET_LUT_LENGTH], brightness_val;

	brightness_val = argsPtr->Backlight_level;
	memcpy(DietFactor, argsPtr->DietFactor, sizeof(argsPtr->DietFactor));

	size = sizeof(DietFactor);
	igt_pipe_obj_replace_prop_blob(pipe, IGT_CRTC_DPST_PIXEL_FACTOR,
			DietFactor, size);
	
	igt_assert_eq(backlight_read(&result, "max_brightness"), 0);
	igt_debug("Brightness value:%d \n",argsPtr->Backlight_level);
	brightness_val = (int)(argsPtr->Backlight_level/10000.0)*result;
	igt_assert_eq(dpst_backlight_write(brightness_val, "brightness"), 0);

	return 0;
}

static DD_DPST_ARGS *send_data_to_DPST_algorithm(igt_display_t *display,
		enum pipe pipe,igt_output_t *output)
{
	drmModePropertyBlobRes *dpst_blob;
	DD_DPST_ARGS *argsPtr = (DD_DPST_ARGS *)malloc(sizeof(DD_DPST_ARGS));
	uint32_t Histogram[DPST_BIN_COUNT], *Histogram_ptr;
	drmModeModeInfo *mode;
	mode = igt_output_get_mode(output);

	igt_info("Waiting for DPST Uevent\n");
	dpst_uevent(25);

	dpst_blob = get_dpst_blob(display->drm_fd, DRM_MODE_OBJECT_CRTC,
			display->pipes[pipe].crtc_id, "DPST Histogram");

	igt_assert_f(dpst_blob,"Failed to read DPST HIstogram Blob\n");

	Histogram_ptr	= (uint32_t *) dpst_blob->data ;
	for(int i =0; i<DPST_BIN_COUNT; i++){
		Histogram[i]= *(Histogram_ptr + i);
		igt_info("Historgram[%d] = %d \n", i, Histogram[i]);
	}

	memcpy(argsPtr->Histogram, Histogram, sizeof(Histogram));
	argsPtr->Aggressiveness_Level = DPST_AGGRESSIVENESS ;
	argsPtr->Resolution_X = mode->hdisplay;
	argsPtr->Resolution_Y = mode->vdisplay;

	igt_debug("Making the call to DPST Alogorithm Library");

	SetHistogramDataBin(argsPtr);
	
	drmModeFreePropertyBlob(dpst_blob);

	return argsPtr;
}

static void enable_DPST_property(int fd, uint32_t type,	uint32_t id,
		bool atomic)
{
	drmModeObjectPropertiesPtr props_dpst, props =
		drmModeObjectGetProperties(fd, id, type);
	int i, ret;
	uint32_t dpst_id;
	drmModeAtomicReqPtr req = NULL;
	igt_assert(props);
	req = drmModeAtomicAlloc();
	for (i = 0; i < props->count_props; i++) {
		uint32_t prop_id = props->props[i];
		uint64_t prop_value = props->prop_values[i];
		drmModePropertyPtr prop = drmModeGetProperty(fd, prop_id);
		igt_assert(prop);

		if(strcmp(prop->name,"DPST"))
			continue;
		igt_debug("prop_id=%d ,property value=%ld,name =%s\n",
				prop_id ,prop_value,prop->name);
		dpst_id = prop_id;
		igt_info("Setting DPST ENUM Property to Enable\n");

		ret = drmModeAtomicAddProperty(req, id, dpst_id, DPST_ENABLE);
		igt_assert(ret >= 0);
		ret = drmModeAtomicCommit(fd, req,
				DRM_MODE_ATOMIC_NONBLOCK, NULL);
		igt_assert_eq(ret, 0);

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
	props_dpst = drmModeObjectGetProperties(fd, id, type);
	for (i = 0; i < props_dpst->count_props; i++) {
		uint32_t prop_id_2 = props_dpst->props[i];
		uint64_t prop_value_2 = props_dpst->prop_values[i];
		drmModePropertyPtr prop_dpst =
			drmModeGetProperty(fd, prop_id_2);

		igt_assert(prop_dpst);
		if(strcmp(prop_dpst->name,"DPST"))
			continue;
		igt_debug("Values After Enabling DPST : prop_id=%d, property value=%ld, name =%s\n",
				prop_id_2 ,prop_value_2,prop_dpst->name);

		igt_assert_f(prop_value_2 == DPST_ENABLE,
				"DPST ENABLE FAILED\n");
		drmModeFreeProperty(prop_dpst);
	}

	drmModeFreeObjectProperties(props_dpst);

	ret = drmModeAtomicCommit(fd, req, 0, NULL);
	igt_assert_eq(ret, 0);
	drmModeAtomicFree(req);
}

static void run_dpst_pipeline(igt_display_t *display,
		enum pipe pipe, igt_output_t *output,bool atomic)
{
	struct igt_fb fb;
	DD_DPST_ARGS *args;

	prepare_pipe(display, pipe, output, &fb);
	igt_info("Enabling DPST on %s (output: %s) \n",
			kmstest_pipe_name(pipe), output->name);
	enable_DPST_property(display->drm_fd,
			DRM_MODE_OBJECT_CRTC, display->pipes[pipe].crtc_id,atomic);

	cleanup_pipe(display, pipe, output, &fb);
	prepare_pipe(display, pipe, output, &fb);

	igt_info("Reading the Histogram Blob on %s (output: %s) and Passing it to the DPST Library \n",
			kmstest_pipe_name(pipe), output->name);
	args = send_data_to_DPST_algorithm(display, pipe, output);

	igt_info("Writing Pixel Factor Blob and Setting Brightness value\n");
	igt_assert_eq(set_pixel_factor_and_brightness(display->pipes,
				args), 0);
	igt_display_commit2(display,
			display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	cleanup_pipe(display, pipe, output, &fb);
}

static void
run_tests_for_dpst(igt_display_t *display,bool atomic)
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
			run_dpst_pipeline(display, pipe, output,atomic);
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
		igt_display_require(&display,display.drm_fd);
	}
	igt_describe("Verifyng  DPST Enablement - Read Histogram Blob - Write Pixel factor blob and Brightness");
	igt_subtest("Enable-DPST")
		run_tests_for_dpst(&display,true);

	igt_fixture {
		igt_display_fini(&display);

	}
}
