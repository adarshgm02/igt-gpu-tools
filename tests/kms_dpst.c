#include "igt.h"
#include "igt_vec.h"
#include "DisplayPcDpst.h"

#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DPST_ENABLE 	1
#define DPST_DISABLE 	0
#define DPST_AGGRESSIVENESS 2

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

void set_pixel_factor(igt_pipe_t *pipe )
{
	DD_DPST_ARGS compargs =Set_BacklightLevel_DietFactor();

	uint32_t DietFactor[DPST_IET_LUT_LENGTH];

	memcpy(DietFactor,compargs.DietFactor ,sizeof(compargs.DietFactor));
	//	uint32_t DietFactor[DPST_IET_LUT_LENGTH] = compargs.DietFactor ;
        size_t size = sizeof(DietFactor);

        igt_pipe_obj_replace_prop_blob(pipe,IGT_CRTC_DPST_PIXEL_FACTOR,DietFactor, size);
}

void send_data_to_DPST_algorithm(igt_display_t *display, enum pipe pipe)
{
	DD_DPST_ARGS args ;
	uint32_t Histogram[DPST_BIN_COUNT];

	drmModePropertyBlobRes *dpst_blob = 
			get_dpst_blob(display->drm_fd,DRM_MODE_OBJECT_CRTC,display->pipes[pipe].crtc_id,"DPST Histogram");

	uint32_t *Histogram_ptr	=(uint32_t *) dpst_blob->data ;

	for(int i =0 ;i<DPST_BIN_COUNT ;i++)
	{
		Histogram[i]= *(Histogram_ptr + i);
		printf("Historgram[%d] = %d \n",i,Histogram[i] );
	}

	args.Aggressiveness_Level = DPST_AGGRESSIVENESS ;

	memcpy(args.Histogram,Histogram,sizeof(Histogram));
	//args.Histogram = Histogram;
	SetHistogramDataBin(args);

	drmModeFreePropertyBlob(dpst_blob);
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
/*	dpst_id	   = props->props[IGT_CRTC_DPST];
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
  */
        for (i = 0; i < props->count_props; i++) {
                uint32_t prop_id = props->props[i];
                uint64_t prop_value = props->prop_values[i];
                drmModePropertyPtr prop = drmModeGetProperty(fd, prop_id);
                igt_assert(prop);
                printf("prop_id=%d ,property value=%ld,name =%s\n",prop_id ,prop_value,prop->name);
                if(strcmp(prop->name,"DPST"))
                        continue;
                printf("Here is the enum valu= %d\n",i);
        //      printf("prop_id=%d ,property value=%ld,name =%s\n",prop_id ,prop_value,prop->name);
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

static void run_crtc_property_for_dpst(igt_display_t *display, enum pipe pipe, igt_output_t *output,bool atomic,const char* test)
{
	struct igt_fb fb;
        prepare_pipe(display, pipe, output, &fb);
       	igt_info("Fetching crtc properties on %s (output: %s)\n", kmstest_pipe_name(pipe), output->name);
	if(strcmp(test,"Enable-DPST")==0)
		test_DPST_properties(display->drm_fd, DRM_MODE_OBJECT_CRTC, display->pipes[pipe].crtc_id,atomic);
	printf("Checking Blob Property : %s \n", test);
	prepare_pipe(display, pipe, output, &fb);
	//drmModePropertyBlobRes *dpst_blob=get_dpst_blob(display->drm_fd,DRM_MODE_OBJECT_CRTC,display->pipes[pipe].crtc_id,"DPST Histogram");
	//drmModeFreePropertyBlob(dpst_blob);
        if(strcmp(test,"Read-DPST-Histogram")==0)
		send_data_to_DPST_algorithm(display,pipe);

        cleanup_pipe(display, pipe, output, &fb);
}

static void
run_tests_for_dpst(igt_display_t *display,bool atomic, const char* test)
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
                       	run_crtc_property_for_dpst(display, pipe, output,atomic,test);
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
                run_tests_for_dpst(&display,true,"Enable-DPST");

	igt_describe("verify if the Histogram Blob Data can be Read and also send the data tp the Algorithm");
        igt_subtest("Read-DPST-Histogram")
                run_tests_for_dpst(&display,true,"Read-DPST-Histogram");
	
	igt_describe("verify if the Pixel Factor Blob Data can be written.");
        igt_subtest("Write-DPS-PixelFactor")
                run_tests_for_dpst(&display,true,"write-DPST Pixel Factor");

        igt_fixture {
                igt_display_fini(&display);
        }
}
