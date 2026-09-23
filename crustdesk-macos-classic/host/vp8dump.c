/* Decode a stream saved by `cdvpoke.py ... save FILE` with libvpx and write
 * the last picture as a PPM: what a peer would be showing.
 *
 *   vp8dump STREAM OUT.ppm
 *
 * STREAM is a run of frames, each a 4-byte big-endian length and the VP8 data.
 */
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include <stdio.h>
#include <stdlib.h>

static int clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int main(int argc, char **argv)
{
    FILE *in, *out;
    vpx_codec_ctx_t dec;
    vpx_image_t *last = NULL;
    unsigned char hdr[4], *buf = malloc(8 << 20);
    int frames = 0, x, y;
    if (argc < 3 || !(in = fopen(argv[1], "rb")))
        return 2;
    if (vpx_codec_dec_init(&dec, vpx_codec_vp8_dx(), NULL, 0))
        return 2;
    while (fread(hdr, 1, 4, in) == 4) {
        unsigned n = (unsigned)hdr[0] << 24 | hdr[1] << 16 | hdr[2] << 8 | hdr[3];
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img;
        if (n > (8u << 20) || fread(buf, 1, n, in) != n)
            break;
        if (vpx_codec_decode(&dec, buf, n, NULL, 0)) {
            fprintf(stderr, "frame %d rejected: %s\n", frames, vpx_codec_error(&dec));
            return 1;
        }
        while ((img = vpx_codec_get_frame(&dec, &it)) != NULL)
            last = img;
        frames++;
    }
    if (!last)
        return 1;
    out = fopen(argv[2], "wb");
    fprintf(out, "P6\n%u %u\n255\n", last->d_w, last->d_h);
    /* BT.601 studio range back to RGB, as a client does. */
    for (y = 0; y < (int)last->d_h; y++)
        for (x = 0; x < (int)last->d_w; x++) {
            int Y = last->planes[0][y * last->stride[0] + x] - 16;
            int U = last->planes[1][(y / 2) * last->stride[1] + x / 2] - 128;
            int V = last->planes[2][(y / 2) * last->stride[2] + x / 2] - 128;
            unsigned char px[3];
            px[0] = (unsigned char)clamp((298 * Y + 409 * V + 128) >> 8);
            px[1] = (unsigned char)clamp((298 * Y - 100 * U - 208 * V + 128) >> 8);
            px[2] = (unsigned char)clamp((298 * Y + 516 * U + 128) >> 8);
            fwrite(px, 1, 3, out);
        }
    fclose(out);
    printf("%d frames decoded, %ux%u\n", frames, last->d_w, last->d_h);
    return 0;
}
