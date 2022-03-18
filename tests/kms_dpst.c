#include "igt.h"
#include "igt_vec.h"
//#include "DisplayPcDpst.h"

#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DPST_ENABLE 	1
#define DPST_DISABLE 	0

bool is_edp  			= false ;
bool is_battery_mode		= false ;
bool is_pwm  			= false ;
bool is_8bpc 			= false ;
bool is_sdr 			= false ;

typedef struct {
        int drm_fd;
        int debugfs_fd;
        uint32_t crtc_id;
        igt_display_t display;
	 drmModeModeInfo *mode;
        igt_output_t *output;
} data_t;



static void setup_output(data_t *data)
{
        igt_output_t *output;
	igt_display_t *display = &data->display;
	enum pipe pipe ;
        for_each_pipe_with_valid_output(display, pipe, output) {
                drmModeConnectorPtr con = output->config.connector;
		printf(" connector type = %d pipe=%d \n",con->connector_type,pipe);

                if (con->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;
		else
			is_edp = true ;
                igt_output_set_pipe(output, pipe);               
                data->output = output;
		data->mode = igt_output_get_mode(output);
                return;
        }
}
static void prepare_pipe(igt_display_t *display, enum pipe pipe, igt_output_t *output, struct igt_fb *fb)
{
        drmModeModeInfo *mode = igt_output_get_mode(output);

        igt_create_pattern_fb(display->drm_fd, mode->hdisplay, mode->vdisplay,
                              DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, fb);

        igt_output_set_pipe(output, pipe);

        igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY), fb);

        igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void cleanup_pipe(igt_display_t *display, enum pipe pipe, igt_output_t *output, struct igt_fb *fb)
{
        igt_plane_t *plane;

        for_each_plane_on_pipe(display, pipe, plane)
                igt_plane_set_fb(plane, NULL);

        igt_output_set_pipe(output, PIPE_NONE);

        igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

        igt_remove_fb(display->drm_fd, fb);
}


static void display_init(data_t *data)
{
        igt_display_require(&data->display, data->drm_fd);
        setup_output(data);
}


static void test_dpst_requirement(void)
{
	/*
	igt_require_f(is_edp ,"The connected Panel is not of tyepe edp\n");
	igt_require_f(is_battery_mode ,"The Display is not in Dc/ Battery Mode");
	igt_require_f(is_pwm,"The connected Panel is not PWM supported\n");
	igt_require_f(is_8bpc ,"The Connected Panel is not of 8bpc\n");
	igt_require_f(is_sdr,"The panel is not in SDR Mode");
	*/
}
static drmModePropertyBlobRes *get_dpst_blob(int fd, uint32_t type, uint32_t id, const char *name )
{
        drmModePropertyBlobRes *blob = NULL;
        uint64_t blob_id;
        int ret;

        ret = kmstest_get_property(fd,
                                   id,
                                   type,
                                   name,
                                   NULL, &blob_id, NULL);
	printf("blob_id : %ld\n",blob_id);
        if (ret)
                blob = drmModeGetPropertyBlob(fd, blob_id);

        igt_assert(blob);
	
	printf("Successfully read the Blob Property");
        return blob;
}

static void test_DPST_properties(int fd, uint32_t type, uint32_t id,bool atomic)
{
        drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(fd, id, type);
        int i, ret;
	uint32_t dpst_id,prop_id;
	uint64_t dpst_value,prop_value;
        drmModeAtomicReqPtr req = NULL;
        igt_assert(props);
        if (atomic)
                req = drmModeAtomicAlloc();
	printf("ENUM =%d\n",IGT_CRTC_DPST);
	dpst_id	   = props->props[IGT_CRTC_DPST];
	dpst_value = props->prop_values[IGT_CRTC_DPST];
	drmModePropertyPtr prop = drmModeGetProperty(fd, dpst_id);
	igt_assert(prop);
	printf("prop_id=%d ,property value=%ld,name =%s\n",dpst_id ,dpst_value,prop->name);
       	printf("Setting DPST Prop value\n");
        if (!atomic) {
		 printf("Entered this block");
	       	 ret = drmModeObjectSetProperty(fd, id, type, dpst_id, DPST_ENABLE);
	         igt_assert_eq(ret, 0);
       	}
        else { printf("Entered the block");
	      printf(" %d\n",DPST_ENABLE);
	      ret = drmModeAtomicAddProperty(req, id, dpst_id, 1);
              igt_assert(ret >= 0);
              ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
              igt_assert_eq(ret, 0);
                }
       	drmModeFreeProperty(prop);
	drmModeFreeObjectProperties(props);
	
	drmModeObjectPropertiesPtr  props1 = drmModeObjectGetProperties(fd, id, type);
	dpst_id    = props1->props[IGT_CRTC_DPST];
	dpst_value = props1->prop_values[IGT_CRTC_DPST];
	drmModePropertyPtr prop1 = drmModeGetProperty(fd, dpst_id);
        igt_assert(prop1);
	
	printf("After :prop_id=%d ,property value=%ld,name =%s\n",i,dpst_id ,dpst_value,prop1->name);
	drmModeFreeProperty(prop1);
        drmModeFreeObjectProperties(props1);
        if (atomic) {
                ret = drmModeAtomicCommit(fd, req, 0, NULL);
                igt_assert_eq(ret, 0);
                drmModeAtomicFree(req);
        }
        
}
static void run_crtc_property_for_dpst(igt_display_t *display, enum pipe pipe, igt_output_t *output,bool atomic)
{
	struct igt_fb fb;
        prepare_pipe(display, pipe, output, &fb);
       	igt_info("Fetching crtc properties on %s (output: %s)\n", kmstest_pipe_name(pipe), output->name);
	//test_DPST_properties(display->drm_fd, DRM_MODE_OBJECT_CRTC, display->pipes[pipe].crtc_id,atomic);
	printf("Checking Blob Property\n");
	drmModePropertyBlobRes *dpst_blob=get_dpst_blob(display->drm_fd,DRM_MODE_OBJECT_CRTC,display->pipes[pipe].crtc_id,"MODE_ID");
	drmModeFreePropertyBlob(dpst_blob);
        cleanup_pipe(display, pipe, output, &fb);
}

static void
run_tests_for_dpst(igt_display_t *display,bool atomic)
{
        bool found_any_valid_pipe = false, found;
        enum pipe pipe;
        igt_output_t *output;
	printf("checkpoint1\n");
	if(atomic)
		igt_skip_on(!display->is_atomic);
	printf("checkpoint2\n");
        for_each_pipe(display, pipe) {
                found = false;
                for_each_valid_output_on_pipe(display, pipe, output) {
                        found_any_valid_pipe = found = true;
                       	run_crtc_property_for_dpst(display, pipe, output,atomic);
                        printf("Checkpoint3\n");
			break;
                }
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
		test_dpst_requirement();
        }
	igt_describe("verify if the DPST can be Enabled and Disabled");
        igt_subtest("Enable-Disable-DPST")
                run_tests_for_dpst(&display,true);

        igt_fixture {
                igt_display_fini(&display);
        }
}
                                                                                                             
