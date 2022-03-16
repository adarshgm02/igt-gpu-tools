#include "igt.h"
#include "igt_vec.h"
//#include "DisplayPcDpst.h"

#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


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
static void test_DPST_properties(int fd, uint32_t type, uint32_t id,bool atomic)
{
        drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(fd, id, type);
        int i, ret;
	uint32_t dpst_id;
	uint64_t dpst_value;
        drmModeAtomicReqPtr req = NULL;
        igt_assert(props);
        if (atomic)
                req = drmModeAtomicAlloc();


        for (i = 0; i < props->count_props; i++) {
                uint32_t prop_id = props->props[i];
                uint64_t prop_value = props->prop_values[i];
                drmModePropertyPtr prop = drmModeGetProperty(fd, prop_id);		
                igt_assert(prop);
		printf("prop_id=%d ,property value=%ld,name =%s\n",prop_id ,prop_value,prop->name);	
		if(strcmp(prop->name,"DPST"))
			continue;
	//	printf("prop_id=%d ,property value=%ld,name =%s\n",prop_id ,prop_value,prop->name);
		dpst_id = prop_id;
		dpst_value =1 ;

                printf("Setting DPST Prop value\n");
      		if (!atomic) {
			ret = drmModeObjectSetProperty(fd, id, type, dpst_id, dpst_value);
                	igt_assert_eq(ret, 0);
		}
		else {
                ret = drmModeAtomicAddProperty(req, id, dpst_id, dpst_value);
                igt_assert(ret >= 0);
              //  ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL)
	        ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
                igt_assert_eq(ret, 0);
                }
                drmModeFreeProperty(prop);
        }
	drmModeFreeObjectProperties(props);
	drmModeObjectPropertiesPtr  props1 = drmModeObjectGetProperties(fd, id, type);
	for (i = 0; i < props1->count_props; i++) {
                uint32_t prop_id1 = props1->props[i];
                uint64_t prop_value1 = props1->prop_values[i];
                drmModePropertyPtr prop1 = drmModeGetProperty(fd, prop_id1);


                igt_assert(prop1);
                if(strcmp(prop1->name,"DPST"))
                        continue;
                printf(" After: prop_id=%d ,property value=%ld,name =%s\n",prop_id1 ,prop_value1,prop1->name);
		drmModeFreeProperty(prop1);
	}

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
	test_DPST_properties(display->drm_fd, DRM_MODE_OBJECT_CRTC, display->pipes[pipe].crtc_id,atomic);
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
                        printf("checkpoint3\n");
			break;
                }
        }

        igt_skip_on(!found_any_valid_pipe);
}

//static data_t data;
igt_main
{
	igt_display_t display;
//	data_t data;
        igt_fixture {
                display.drm_fd = drm_open_driver_master(DRIVER_ANY);

                kmstest_set_vt_graphics_mode();
               // igt_require_pipe_crc(data.drm_fd);
                igt_display_require(&display,display.drm_fd);
		test_dpst_requirement();
	//	display_init(&data);
        }
	igt_describe("verify if the DPST can be Enabled and Disabled");
        igt_subtest("Enable-Disable-DPST")
                run_tests_for_dpst(&display,true);

        igt_fixture {
                igt_display_fini(&display);
        }
}
                                                                                                             
