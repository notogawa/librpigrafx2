#include <interface/mmal/mmal.h>
#include "../include/rpigrafx.h"

#define MAX_CAMERAS MMAL_PARAMETER_CAMERA_INFO_MAX_CAMERAS
#define NUM_SPLITTER_OUTPUTS 4

static int32_t num_cameras = 0;

static MMAL_COMPONENT_T *cp_cameras[MAX_CAMERAS];
static struct {
    _Bool is_used;
    int32_t width, height;
    int32_t max_width, max_height;
} cameras_config[MAX_CAMERAS];

static MMAL_COMPONENT_T *cp_splitters[MAX_CAMERAS];
static struct {
    int next_output_idx;
} splitters_config[MAX_CAMERAS];

static MMAL_COMPONENT_T *cp_isps[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];
static struct {
    int32_t width, height;
    _Bool is_zero_copy_rendering;
} isps_config[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];

static MMAL_CONNECTION_T *conn_camera_splitters[MAX_CAMERAS];
static MMAL_CONNECTION_T *conn_splitters_isps[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];

struct callback_context {
    MMAL_STATUS_T status;
    MMAL_BUFFER_HEADER_T *header;
    vcos_semaphore_t sem_set_buffer, sem_rcvd_buffer;
};

int priv_rpigrafx_mmal_init()
{
    int i, j;
    int ret = 0;
    MMAL_STATUS_T status;

    if (called.mmal != 0)
        goto end;

    for (i = 0; i < MAX_CAMERAS; i ++) {
        cp_cameras[i] = cp_splitters[i] = NULL;
        conn_camera_splitters[i] = NULL;
        for (j = 0; j < NUM_SPLITTER_OUTPUTS; j ++) {
            cp_isps[i][j] = NULL;
            conn_splitters_isps[i][j] = NULL;
        }
        cameras_config[i].width  = 0;
        cameras_config[i].height = 0;
        splitters_config[i].next_idx = 0;
    }

    {
        MMAL_COMPONENT_T *cp_camera_info = NULL;
        MMAL_PARAMETER_CAMERA_INFO_T camera_info = {
            .hdr = {
                .id = MMAL_PARAMETER_CAMERA_INFO;
                .size = sizeof(camera_info)
            }
        };

        status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO,
                                       &cp_camera_info);
        if (status != MMAL_SUCCESS) {
            print_error("Creating camera_info component failed: 0x%08x", status);
            cp_camera_info = NULL;
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_get(cp_camera_info->control, &camera_info.hdr);
        if (status != MMAL_SUCCESS) {
            print_error("Getting camera info failed: 0x%08x", status);
            ret = 1;
            goto end;
        }

        num_cameras = camera_info.num_cameras;
        if (num_cameras <= 0) {
            print_error("No cameras found: 0x%08x", num_cameras);
            ret = 1;
            goto end;
        }

        for (i = 0; i < num_cameras; i ++) {
            cameras_config[i].max_width  = camera_info.cameras[i].max_width;
            cameras_config[i].max_height = camera_info.cameras[i].max_height;
        }
        for (; i < MAX_CAMERAS; i ++) {
            cameras_config[i].max_width  = 0;
            cameras_config[i].max_height = 0;
        }

        status = mmal_component_destroy(cp_camera_info);
        if (status != MMAL_SUCCESS) {
            print_error("Destroying camera_info component failed: 0x%08x", status);
            cp_camera_info = NULL;
            ret = 1;
            goto end;
        }
    }

end:
    called.mmal ++;

    return ret;
}

int priv_rpigrafx_mmal_finalize()
{
    int i, j;

    if (called.mmal != 1)
        goto skip;

    for (i = 0; i < MAX_CAMERAS; i ++) {
        cp_cameras[i] = cp_splitters[i] = NULL;
        for (j = 0; j < NUM_SPLITTER_OUTPUTS; j ++)
            cp_isps[i][j] = NULL;
        cameras_config[i].width  = -1;
        cameras_config[i].height = -1;
        cameras_config[i].max_width  = -1;
        cameras_config[i].max_height = -1;
        splitters_config[i].next_idx = 0;
    }

skip:
    called.mmal --;
}

static void callback_control(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *header)
{
    MMAL_PARAM_UNUSED(port);
    mmal_buffer_header_release(buffer);
}

static void callback_isp_output(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *header)
{
    struct callback_context *ctx = (struct callback_context*) port->user_data;

    ctx->header = header;
    vcos_semaphore_post(&ctx->sem);
}

int rpigrafx_config_camera_frame(const int32_t camera_number,
                                 const int32_t width, const int32_t height,
                                 const MMAL_FOURCC_T encoding,
                                 const _Bool is_zero_copy_rendering
                                 rpigrafx_frame_config_t *fcp)
{
    int32_t max_width, max_height;
    int idx;
    int ret = 0;

    if (camera_number >= num_cameras) {
        print_error("camera_number(%d) exceeds num_cameras(%d)", camera_number, num_cameras);
        ret = 1;
        goto end;
    }
    max_width  = cameras_config[camera_number].max_width;
    max_height = cameras_config[camera_number].max_height;
    if (width > max_width) {
        print_error("width(%d) exceeds max_width(%d) of camera %d",
                    width, max_width, camera_number);
        ret = 1;
        goto end;
    } else if (height > max_height) {
        print_error("height(%d) exceeds max_height(%d) of camera %d",
                    width, max_width, camera_number);
        ret = 1;
        goto end;
    }

    /*
     * Only set use flag here.
     * cameras_config[camera_number].{width,height}
     * will be set on rpigrafx_finish_config.
     */
    cameras_config[camera_number].is_used = !0;

    if (splitters_config[camera_number].next_output_idx == NUM_SPLITTER_OUTPUTS - 1) {
        print_error("Too many splitter clients(%d) of camera %d",
                    splitters_config[camera_number].next_output_idx,
                    camera_number);
        ret = 1;
        goto end;
    }
    idx = splitters_config[camera_number].next_output_idx ++;

    isps_config[camera_number][idx].width  = width;
    isps_config[camera_number][idx].height = height;
    isps_config[camera_number][idx].is_zero_copy_rendering = is_zero_copy_rendering;

    fcp->camera_number = camera_number;
    fcp->splitter_output_port_index = idx;
    fcp->is_zero_copy_rendering = is_zero_copy_rendering;

end:
    return ret;
}

int rpigrafx_finish_config()
{
    int i, j;
    int ret = 0;

    for (i = 0; i < num_cameras; i ++) {
        int len;
        /* Maximum width/height of the requested frames. */
        int32_t max_width, max_height;

        if (!cp_cameras[i].is_used)
            continue;

        len = splitters_config[i].next_output_idx;

        max_width = max_height = 0;
        for (j = 0; j < len; j ++) {
            max_width  = MMAL_MAX(max_width,  isps_config[i][j].width);
            max_height = MMAL_MAX(max_height, isps_config[i][j].height);
        }

        status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &cp_cameras[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Creating camera component of camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
        {
            MMAL_PORT_T *control = mmal_util_get_port(cp_cameras[i],
                                                      MMAL_PORT_TYPE_CONTROL, 0);

            if (control == NULL) {
                print_error("Getting control port of camera %d failed", i);
                ret = 1;
                goto end;
            }

            status = mmal_port_parameter_set_int32(control, MMAL_PARAMETER_CAMERA_NUM, i);
            if (status != MMAL_SUCCESS) {
                print_error("Setting camera_num of camera %d failed: 0x%08x", i, status);
                ret = 1;
                goto end;
            }

            status = mmal_port_enable(control, callback_control);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling control port of camera %d failed: 0x%08x", i, status);
                ret = 1;
                goto end;
            }
        }
        {
            MMAL_PORT_T *output = mmal_util_get_port(cp_cameras[i],
                                                     MMAL_PORT_TYPE_OUTPUT, 0);

            if (output == NULL) {
                print_error("Getting output port of camera %d failed", i);
                ret = 1;
                goto end;
            }

            status = config_port(output, MMAL_ENCODING_OPAQUE, max_width, max_height);
            if (status != MMAL_SUCCESS) {
                print_error("Setting format of camera %d failed: 0x%08x", i, status);
                ret = 1;
                goto end;
            }

            status = mmal_parameter_set_boolean(output,
                                                MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
            if (status != MMAL_SUCCESS) {
                print_error("Setting zero-copy on camera %d failed: 0x%08x", i, status);
                ret = 1;
                goto end;
            }
        }
        status = mmal_component_enable(cp_camera[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling camera component of camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }

        status = mmal_component_create(MMAL_COMPONENT_VIDEO_SPLITTER, &cp_splitters[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Creating splitter component of camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
        {
            MMAL_PORT_T *control = mmal_util_get_port(cp_splitters[i],
                                                      MMAL_PORT_TYPE_CONTROL, 0);

            if (control == NULL) {
                print_error("Getting control port of splitter %d failed", i);
                ret = 1;
                goto end;
            }

            status = mmal_port_enable(control, callback_control);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling control port of splitter %d failed: 0x%08x", i, status);
                ret = 1;
                goto end;
            }
        }
        for (j = 0; j < len; j ++) {
            MMAL_PORT_T *input = mmal_util_get_port(cp_splitters[i],
                                                    MMAL_PORT_TYPE_INPUT, j);

            if (input == NULL) {
                print_error("Getting input port of splitter %d,%d failed", i, j);
                ret = 1;
                goto end;
            }

            status = config_port(input, MMAL_ENCODING_OPAQUE, max_width, max_height);
            if (status != MMAL_SUCCESS) {
                print_error("Setting format of " \
                            "splitter %d input %d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }

            status = mmal_parameter_set_boolean(input,
                                                MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
            if (status != MMAL_SUCCESS) {
                print_error("Setting zero-copy on " \
                            "splitter %d input %d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }
        for (j = 0; j < len; j ++) {
            MMAL_PORT_T *output = mmal_util_get_port(cp_splitters[i],
                                                     MMAL_PORT_TYPE_OUTPUT, j);

            if (output == NULL) {
                print_error("Getting output port of splitter %d,%d failed", i, j);
                ret = 1;
                goto end;
            }

            status = config_port(output, MMAL_ENCODING_OPAQUE, max_width, max_height);
            if (status != MMAL_SUCCESS) {
                print_error("Setting format of " \
                            "splitter %d output %d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }

            status = mmal_parameter_set_boolean(output,
                                                MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
            if (status != MMAL_SUCCESS) {
                print_error("Setting zero-copy on " \
                            "splitter %d output %d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }
        status = mmal_component_enable(cp_splitter[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling splitter component of camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }

        for (j = 0; j < len; j ++) {
            status = mmal_component_create("vc.ril.isp", &cp_isps[i][j]);
            if (status != MMAL_SUCCESS) {
                print_error("Creating isp component %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
            {
                MMAL_PORT_T *control = mmal_util_get_port(cp_isps[i][j],
                                                          MMAL_PORT_TYPE_CONTROL, 0);

                if (control == NULL) {
                    print_error("Getting control port of isp %d,%d failed", i, j);
                    ret = 1;
                    goto end;
                }

                status = mmal_port_enable(control, callback_control);
                if (status != MMAL_SUCCESS) {
                    print_error("Enabling control port of " \
                                "isp %d,%d failed: 0x%08x", i, j, status);
                    ret = 1;
                    goto end;
                }
            }
            {
                MMAL_PORT_T *input = mmal_util_get_port(cp_isps[i][j],
                                                        MMAL_PORT_TYPE_INPUT, j);

                if (input == NULL) {
                    print_error("Getting input port of isp %d,%d failed", i, j);
                    ret = 1;
                    goto end;
                }

                status = config_port(input, MMAL_ENCODING_OPAQUE, max_width, max_height);
                if (status != MMAL_SUCCESS) {
                    print_error("Setting format of " \
                                "isp %d input %d failed: 0x%08x", i, j, status);
                    ret = 1;
                    goto end;
                }

                status = mmal_parameter_set_boolean(input,
                                                    MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
                if (status != MMAL_SUCCESS) {
                    print_error("Setting zero-copy on " \
                                "isp %d input %d failed: 0x%08x", i, j, status);
                    ret = 1;
                    goto end;
                }
            }
            {
                MMAL_PORT_T *output = mmal_util_get_port(cp_isps[i][j],
                                                         MMAL_PORT_TYPE_OUTPUT, j);

                if (output == NULL) {
                    print_error("Getting output port of isp %d,%d failed", i, j);
                    ret = 1;
                    goto end;
                }

                status = config_port(output,
                                     isps_config[i][j].encoding,
                                     isps_config[i][j].width,
                                     isps_config[i][j].height);
                if (status != MMAL_SUCCESS) {
                    print_error("Setting format of " \
                                "isp %d output %d failed: 0x%08x", i, j, status);
                    ret = 1;
                    goto end;
                }

                status = mmal_parameter_set_boolean(output,
                                                    MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
                if (status != MMAL_SUCCESS) {
                    print_error("Setting zero-copy on " \
                                "isp %d output %d failed: 0x%08x", i, j, status);
                    ret = 1;
                    goto end;
                }

                pool_isp[i][j] = mmal_port_pool_create(output,
                                                       output->buffer_num,
                                                       output->buffer_size);
                if (pool_isp[i][j] == NULL) {
                    print_error("Creating pool of isp component %d,%d failed", i, j);
                    ret = 1;
                    goto end;
                }
            }
            status = mmal_component_enable(cp_isp[i][j]);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling isp component %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }

        status = mmal_connection_create(&conn_camera_splitters[i],
                                        cp_cameras[i]->output[0],
                                        cp_splitters[i]->input[0],
                                        MMAL_CONNECTION_FLAG_TUNNELLING);
        if (status != MMAL_SUCCESS) {
            print_error("Connecting " \
                        "camera and splitter ports %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
        for (j = 0; j < len; j ++) {
            status = mmal_connection_create(&conn_splitters_isps[i][j],
                                            cp_splitters[i]->output[j],
                                            cp_isps[i][j]->input[0],
                                            MMAL_CONNECTION_FLAG_TUNNELLING);
            if (status != MMAL_SUCCESS) {
                print_error("Connecting " \
                            "splitter and isp ports %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }

        for (j = 0; j < len; j ++) {
            status = mmal_port_enable(cp_isps[i][j]->output[0], callback_isp_output);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling isp port %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }

        for (j = 0; j < len; j ++) {
            status = mmal_connection_enable(conn_splitters_isps[i][j]);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling connection between " \
                            "splitter and isp %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }
        status = mmal_connection_enable(conn_camera_splitters[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling connection between " \
                        "camera and splitter %d,%d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }

end:
    return ret;
}

void* rpigrafx_get_frame(rpigrafx_frame_config_t fc)
{
    const int i = fc.camera_number;
    const int j = fc.splitter_output_port_index;
    MMAL_COMPONENT_T *output = NULL;
    struct callback_context *ctx = NULL;
    void *ret = NULL;

    output = mmal_util_get_port(cp_isps[i][j], MMAL_PORT_TYPE_OUTPUT, 0);
    if (output == NULL) {
        print_error("Getting output port of isp %d,%d failed", i, j);
        ret = NULL;
        goto end;
    }

    ctx = &isps_ctx[i][j];
    vcos_semaphore_wait(ctx->sem_sent_header);
    if (ctx->status != MMAL_SUCCESS) {
        print_error("Getting output buffer of isp %d,%d failed: 0x%08x", i, j, ctx->status);
        ret = NULL;
        goto end;
    }
    ret = ctx->header->buffer;
    vcos_semaphore_post(ctx->sem_rcvd_header);

end:
    return ret;
}

int rpigrafx_register_frame_pool_to_qmkl(rpigrafx_frame_config_t fc);

int rpigrafx_config_render(const _Bool is_fullscreen,
                           const int32_t x, const int32_t y,
                           const int32_t width, const int32_t height,
                           const int32_t layer,
                           rpigrafx_render_config_t *rcp);

int rpigrafx_render_frane(rpigrafx_frame_cofig_t fc,
                          rpigrafx_render_config_t rc);