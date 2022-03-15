#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#if defined(__linux__)
# include <linux/videodev2.h>
#else
# include <sys/videoio.h>
#endif

#include <Eina.h>
#include <Ecore.h>
#include <Evas.h>
#include <Ecore_Evas.h>

#define RGB_VALID(x) ((x) < 0) ? 0 :(((x) > 255) ? 255: (x))

static void
borked(const char *why)
{
    fprintf(stderr, "ERROR: %s\n", why);
    exit(1);
}

static void
yuvtorgb(int Y, int U, int V, unsigned char *rgb)
{
    int r, g, b;
    static short L1[256], L2[256], L3[256], L4[256], L5[256];
    static int initialised;

    if (!initialised) {
        initialised=1;
        for (int i = 0; i < 256 ; i++) {
            L1[i] = 1.164*(i-16);
            L2[i] = 1.596*(i-128);
            L3[i] = -0.813*(i-128);
            L4[i] = 2.018*(i-128);
            L5[i] = -0.391*(i-128);
        }
    }

    r = L1[Y] + L2[V];
    g = L1[Y] + L3[U] + L5[V];
    b = L1[Y] + L4[U];

    rgb[0] = RGB_VALID(b);
    rgb[1] = RGB_VALID(g);
    rgb[2] = RGB_VALID(r);
    rgb[3] = 255;
}

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

static bool
save_photo(const char *data, unsigned int w, unsigned int h)
{
    Ecore_Evas *ee;
    Evas *evas;
    Evas_Object *o;
    struct tm *info;
    time_t rawtime;
    char buf[256];
    char filename[4096];
    struct timespec ts;

    time(&rawtime);
    info = localtime(&rawtime);
    strftime(buf, sizeof(buf), "%F-%T", info);
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(filename, sizeof(filename), "images/%s:%ld.jpg", buf, ts.tv_nsec);

    ee = ecore_evas_new(NULL, 0, 0, 1, 1, NULL);
    if (!ee) return false;
    evas = ecore_evas_get(ee);
    o = evas_object_image_filled_add(evas);
    if (!o) return false;
    evas_object_image_size_set(o, w, h);
    evas_object_image_colorspace_set(o, EVAS_COLORSPACE_ARGB8888);
    evas_object_image_data_set(o, (void *)data);
    evas_object_resize(o, w, h);
    evas_object_move(o, 0, 0);
    evas_object_image_save(o, filename, NULL, NULL);
    evas_object_del(o);
    ecore_evas_free(ee);

    return true;
}

typedef struct {
    char      *start;
    uint32_t  length;
} buffer_t;

static int
mmap_camera(int fd, int w, int h)
{
    fd_set fds, readset;
    struct timeval tv;
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers req;
    struct termios ntio, otio;
    time_t ts, te;
    int i, type = V4L2_BUF_TYPE_VIDEO_CAPTURE, count = 0;

    memset(&req, 0, sizeof(req));
    req.count = 16;
    req.type= V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        borked("VIDIOC_REQBUFS");
    }

    buffer_t *buffers = calloc(req.count, sizeof(buffer_t));
    if (!buffers) {
        borked("calloc()");
    }

    for (i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
            borked("VIDIOC_QUERYBUF");
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            borked("mmap");
        }
    }

    for (i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            borked("VIDIOC_QBUF");
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        borked("VIDIOC_STREAMON");
    }

    char *out = malloc((w * h) * sizeof(uint32_t));
    if (!out) {
        borked("malloc");
    }

    tcgetattr(STDIN_FILENO, &otio);
    ntio = otio;
    ntio.c_lflag &= (~ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &ntio);

    ts = time(NULL);
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    FD_SET(STDIN_FILENO, &fds);

    while (1) {
                readset = fds;
        int n = select(fd + 1, &readset, NULL, NULL, NULL);
        if (n == -1 && errno == EINTR) continue;
        else if (n == 0)
            borked("timeout");

        if (FD_ISSET(STDIN_FILENO, &readset))
            break;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) continue;
            borked("VIDIOC_DQBUF");
        }

        YUV422toBGRA(buffers[buf.index].start, out, w, h);
        if (!save_photo(out, w, h)) break;
        count++;

        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            borked("VIDIOC_QBUF");
        }
    }

    free(out);

    te = time(NULL);

    printf("Saved %d frames in %lld secs (%1.1f FPS).\n", count, te - ts, (double) count / (te - ts));
    tcsetattr(STDIN_FILENO, TCSANOW, &otio);

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMOFF, &type) == -1) {
        borked("VIDIOC_STREAMON");
    }

    for (i = 0; i < req.count; i++) {
        munmap(buffers[i].start, buffers[i].length);
    }

    return 0;
}

static void
usage(void)
{
    printf("Usage: ./cam <device>\n");
    exit(0);
}

int main(int argc, char **argv)
{
    struct v4l2_capability cap;
    struct v4l2_format format;
    uint32_t w, h;
    int fd, ret = 1;

    ecore_evas_init();

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
    format.fmt.pix.width = 1920;
    format.fmt.pix.height = 1080;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

    if (ioctl(fd, VIDIOC_S_FMT, &format) == -1) {
        fprintf(stderr, "VIDIOC_S_FMT: %s\n", strerror(errno));
        goto cleanup;
    }

    if (ioctl(fd, VIDIOC_G_FMT, &format) == -1) {
        fprintf(stderr, "VIDIOC_G_FMT: %s\n", strerror(errno));
        goto cleanup;
    }

    w = format.fmt.pix.width;
    h = format.fmt.pix.height;

    switch (format.fmt.pix.pixelformat) {
        case V4L2_PIX_FMT_YUYV:
            ret = mmap_camera(fd, w, h);
        break;
        default:
        break;
    }

cleanup:
    if (fd != -1)
        close(fd);
    ecore_evas_shutdown();

    return ret;
}
