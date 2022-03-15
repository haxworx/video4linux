#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#if defined(__linux__)
# include <linux/videodev2.h>
#else
# include <sys/videoio.h>
#endif

#include <Eina.h>
#include <Ecore.h>
#include <Evas.h>
#include <Ecore_Evas.h>
#include <Elementary.h>

#define PROGRAM_NAME "vid"

typedef struct _Cam {
    int            fd;

    Ecore_Thread  *thread;

    Evas_Object   *o;
    Evas_Coord     w;
    Evas_Coord     h;
    void          *data;
} Cam;

static int mmap_camera(Cam *cam);

static void
_win_del_cb(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
    Cam *cam = data;

    ecore_thread_cancel(cam->thread);
    ecore_thread_wait(cam->thread, 1.0);
    ecore_main_loop_quit();
}

static Evas_Object *
_win_add(Evas_Coord w, Evas_Coord h)
{
    Evas_Object *win = elm_win_util_standard_add("ok", "ok");

    evas_object_resize(win, w, h);
    elm_win_title_set(win, "Camerosa");
    elm_win_center(win, EINA_TRUE, EINA_TRUE);
    evas_object_show(win);

    return win;
}

static Cam *
_camera_add(int fd, Evas_Coord w, Evas_Coord h)
{
    Cam *cam;
    Evas_Object *win, *o;

    cam = calloc(1, sizeof(Cam));
    if (!cam) return NULL;
    cam->fd = fd;
    cam->w = w;
    cam->h = h;

    win = _win_add(w, h);
    evas_object_smart_callback_add(win, "delete,request", _win_del_cb, cam);
    cam->o = o = evas_object_image_filled_add(evas_object_evas_get(win));
    evas_object_image_size_set(o, w, h);
    evas_object_image_colorspace_set(o, EVAS_COLORSPACE_ARGB8888);
    evas_object_resize(o, w, h);
    evas_object_move(o, 0, 0);
    elm_win_resize_object_add(win, o);

    return cam;
}

static void
_thread_run_cb(void *data, Ecore_Thread *thread)
{
    Cam *cam = data;
    mmap_camera(cam);
}

static void
_thread_feedback_cb(void *data, Ecore_Thread *thread, void *msg)
{
    Cam *cam = data;

    if (ecore_thread_check(thread)) return;

    evas_object_image_data_set(cam->o, cam->data);
    evas_object_show(cam->o);
    evas_object_image_data_update_add(cam->o, 0, 0, cam->w, cam->h);
}

static void
_thread_end_cb(void *data, Ecore_Thread *thread)
{
    (void) data; (void) thread;
}

#define RGB_VALID(x) ((x) < 0) ? 0 :(((x) > 255) ? 255: (x))

static void
YUV444toBGRA(uint8_t Y, uint8_t U, uint8_t V, uint8_t *rgb)
{
    int r, g, b;

    r = Y + 1.402 * (V - 128);
    g = Y - 0.344 * (U - 128) - (0.714 * (V - 128));
    b = Y + 1.772 * (U - 128);

    rgb[0] = RGB_VALID(b);
    rgb[1] = RGB_VALID(g);
    rgb[2] = RGB_VALID(r);
    rgb[3] = 255;
}

void
YUV422toBGRA(unsigned char *buf, char *rgb, unsigned int w, unsigned int h)
{
    for (int i = 0; i < w * h; i += 2) {
        uint8_t Y1, Y2, U, V;

        Y1 = buf[2*i+0];
        Y2 = buf[2*i+2];
        U = buf[2*i+1];
        V = buf[2*i+3];

        YUV444toBGRA(Y1, U, V, &rgb[4*i]);
        YUV444toBGRA(Y2, U, V, &rgb[4*(i+1)]);
    }
}

typedef struct {
    char     *start;
    uint32_t  length;
} buffer_t;

static int
mmap_camera(Cam *cam)
{
    fd_set fds, readset;
    struct timeval tv;
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers req;
    time_t ts, te;
    int i, count, fd, w, h, type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    fd = cam->fd; w = cam->w; h = cam->h;

    memset(&req, 0, sizeof(req));
    req.count = 16;
    req.type= V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        fprintf(stderr, "VIDIOC_REQBUFS\n");
        exit(EXIT_FAILURE);
    }

    buffer_t *buffers = calloc(req.count, sizeof(buffer_t));
    if (!buffers) {
        fprintf(stderr, "calloc()\n");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
            fprintf(stderr, "VIDIOC_QUERYBUF\n");
            exit(EXIT_FAILURE);
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "mmap\n");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            fprintf(stderr, "VIDIOC_QBUF\n");
            exit(EXIT_FAILURE);
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        fprintf(stderr, "VIDIOC_STREAMON\n");
        exit(EXIT_FAILURE);
    }

    char *out = malloc((w * h) * sizeof(uint32_t));
    if (!out) {
        fprintf(stderr, "malloc\n");
        exit(EXIT_FAILURE);
    }

    count = 0;
    ts = time(NULL);

    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    while (1) {
        readset = fds;

        int n = select(fd + 1, &readset, NULL, NULL, NULL);
        if (n == -1 && errno == EINTR) continue;
        else if (n == 0) {
            fprintf(stderr, "select timeout\n");
            exit(EXIT_FAILURE);
        }

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN)
                continue;
            else {
                fprintf(stderr, "VIDIOC_DQBUF\n");
                exit(EXIT_FAILURE);
            }
        }

        YUV422toBGRA(buffers[buf.index].start, out, w, h);
        cam->data = out;
        ecore_thread_feedback(cam->thread, cam);
        count++;

        if (ioctl(fd, VIDIOC_QBUF, &buf)) {
                fprintf(stderr, "VIDIOC_QBUF\n");
                exit(EXIT_FAILURE);
        }

        if (ecore_thread_check(cam->thread)) break;
    }

    free(out);

    te = time(NULL);

    printf("%d frames in %lld secs (%1.1f FPS).\n", count, te - ts, (double) count / (te - ts));

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMOFF, &type) == -1) {
        fprintf(stderr, "VIDIOC_STREAMOFF\n");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < req.count; i++) {
        munmap(buffers[i].start, buffers[i].length);
    }

    return 0;
}

static void
usage(void)
{
    printf("Usage: %s <device>\n", PROGRAM_NAME);
    exit(0);
}

int main(int argc, char **argv)
{
    Cam *cam = NULL;
    struct v4l2_capability cap;
    struct v4l2_format format;
    uint32_t w, h;
    int fd, ret = 1;

    eina_init();
    ecore_init();
    elm_init(argc, argv);

    if (argc != 2) {
        usage();
    }

    fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "Unable to open %s - %s\n", argv[1], strerror(errno));
        goto cleanup;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        fprintf(stderr, "VIDIOC_QUERYCAP: %s\n", strerror(errno));
        goto cleanup;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "NO STREAMING CAPABILITY\n");
        goto cleanup;
    }

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = 640;
    format.fmt.pix.height = 480;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

    if (ioctl(fd, VIDIOC_S_FMT, &format) == -1) {
        fprintf(stderr, "VIDIOC_S_FMT: %s\n", strerror(errno));
        goto cleanup;
    }

    if (ioctl(fd, VIDIOC_G_FMT, &format) == -1) {
        fprintf(stderr, "VIDIOC_G_FMT: %s\n", strerror(errno));
        goto cleanup;
    }

    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        fprintf(stderr, "Unsupported pixel format.\n");
        goto cleanup;
    }

    w = format.fmt.pix.width;
    h = format.fmt.pix.height;
    ret = 0;

    cam = _camera_add(fd, w, h);
    cam->thread = ecore_thread_feedback_run(_thread_run_cb, _thread_feedback_cb, _thread_end_cb, _thread_end_cb, cam, EINA_FALSE);
    ecore_main_loop_begin();
cleanup:

    if (cam)
        free(cam);

    if (fd != -1)
        close(fd);

    elm_shutdown();
    ecore_shutdown();
    eina_shutdown();

    return ret;
}
