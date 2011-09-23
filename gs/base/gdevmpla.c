/* Copyright (C) 2001-2006 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied, modified
   or distributed except as expressly authorized under the terms of that
   license.  Refer to licensing information at http://www.artifex.com/
   or contact Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134,
   San Rafael, CA  94903, U.S.A., +1(415)492-9861, for further information.
*/
/* $Id$ */
/* Any-depth planar "memory" (stored bitmap) device */
#include "memory_.h"
#include "gx.h"
#include "gserrors.h"
#include "gsbitops.h"
#include "gxdevice.h"
#include "gxdevmem.h"           /* semi-public definitions */
#include "gxgetbit.h"
#include "gdevmem.h"            /* private definitions */
#include "gdevmpla.h"           /* interface */
#include "gxdevsop.h"

/* procedures */
static dev_proc_open_device(mem_planar_open);
declare_mem_procs(mem_planar_copy_mono, mem_planar_copy_color, mem_planar_fill_rectangle);
static dev_proc_copy_color(mem_planar_copy_color_24to8);
static dev_proc_copy_color(mem_planar_copy_color_4to1);
static dev_proc_copy_plane(mem_planar_copy_plane);
static dev_proc_strip_tile_rectangle(mem_planar_strip_tile_rectangle);
static dev_proc_strip_copy_rop(mem_planar_strip_copy_rop);
static dev_proc_get_bits_rectangle(mem_planar_get_bits_rectangle);

/* It's a bit nasty to have to fork the planar dev_spec_op like this, but
 * the forwarding nature of the device means that the function pointer test
 * for the map_cmyk_color call fails if we let it fall through to the
 * default device. */
static int
mem_planar_dev_spec_op(gx_device *pdev, int dev_spec_op,
                       void *data, int size)
{
    if (dev_spec_op == gxdso_is_native_planar)
        return 1;
    return gx_default_dev_spec_op(pdev, dev_spec_op, data, size);
}

static int
mem_planar_dev_spec_op_cmyk4(gx_device *pdev, int dev_spec_op,
                             void *data, int size)
{
    if (dev_spec_op == gxdso_is_native_planar)
        return 1;
    if (dev_spec_op == gxdso_is_std_cmyk_1bit)
        return 1;
    return gx_default_dev_spec_op(pdev, dev_spec_op, data, size);
}

/*
 * Set up a planar memory device, after calling gs_make_mem_device but
 * before opening the device.  The pre-existing device provides the color
 * mapping procedures, but not the drawing procedures.  Requires: num_planes
 * > 0, plane_depths[0 ..  num_planes - 1] > 0, sum of plane depths =
 * mdev->color_info.depth.
 *
 * Note that this is the only public procedure in this file, and the only
 * sanctioned way to set up a planar memory device.
 */
int
gdev_mem_set_planar(gx_device_memory * mdev, int num_planes,
                    const gx_render_plane_t *planes /*[num_planes]*/)
{
    int total_depth;
    int same_depth = planes[0].depth;
    gx_color_index covered = 0;
    int pi;
    const gx_device_memory *mdproto = gdev_mem_device_for_bits(mdev->color_info.depth);

    if (num_planes < 1 || num_planes > GX_DEVICE_COLOR_MAX_COMPONENTS)
        return_error(gs_error_rangecheck);
    for (pi = 0, total_depth = 0; pi < num_planes; ++pi) {
        int shift = planes[pi].shift;
        int plane_depth = planes[pi].depth;
        gx_color_index mask;

        if (shift < 0 || plane_depth > 16 ||
            !gdev_mem_device_for_bits(plane_depth))
            return_error(gs_error_rangecheck);
        mask = (((gx_color_index)1 << plane_depth) - 1) << shift;
        if (covered & mask)
            return_error(gs_error_rangecheck);
        covered |= mask;
        if (plane_depth != same_depth)
            same_depth = 0;
        total_depth += plane_depth;
    }
    if (total_depth > mdev->color_info.depth)
        return_error(gs_error_rangecheck);
    mdev->num_planes = num_planes;
    memcpy(mdev->planes, planes, num_planes * sizeof(planes[0]));
    mdev->plane_depth = same_depth;
    /* Change the drawing procedures. */
    set_dev_proc(mdev, open_device, mem_planar_open);
    if (num_planes == 1) {
        /* For 1 plane, just use a normal device */
        set_dev_proc(mdev, fill_rectangle, dev_proc(mdproto, fill_rectangle));
        set_dev_proc(mdev, copy_mono,  dev_proc(mdproto, copy_mono));
        set_dev_proc(mdev, copy_color, dev_proc(mdproto, copy_color));
        set_dev_proc(mdev, copy_alpha, dev_proc(mdproto, copy_alpha));
        set_dev_proc(mdev, strip_tile_rectangle, dev_proc(mdproto, strip_tile_rectangle));
        set_dev_proc(mdev, strip_copy_rop, dev_proc(mdproto, strip_copy_rop));
        set_dev_proc(mdev, get_bits_rectangle, dev_proc(mdproto, get_bits_rectangle));
    } else {
        set_dev_proc(mdev, fill_rectangle, mem_planar_fill_rectangle);
        set_dev_proc(mdev, copy_mono, mem_planar_copy_mono);
        set_dev_proc(mdev, dev_spec_op, mem_planar_dev_spec_op);
        if ((mdev->color_info.depth == 24) &&
            (mdev->num_planes == 3) &&
            (mdev->planes[0].depth == 8) && (mdev->planes[0].shift == 16) &&
            (mdev->planes[1].depth == 8) && (mdev->planes[1].shift == 8) &&
            (mdev->planes[2].depth == 8) && (mdev->planes[2].shift == 0))
            set_dev_proc(mdev, copy_color, mem_planar_copy_color_24to8);
        else if ((mdev->color_info.depth == 4) &&
                 (mdev->num_planes == 4) &&
                 (mdev->planes[0].depth == 1) && (mdev->planes[0].shift == 3) &&
                 (mdev->planes[1].depth == 1) && (mdev->planes[1].shift == 2) &&
                 (mdev->planes[2].depth == 1) && (mdev->planes[2].shift == 1) &&
                 (mdev->planes[3].depth == 1) && (mdev->planes[3].shift == 0)) {
            set_dev_proc(mdev, copy_color, mem_planar_copy_color_4to1);
            set_dev_proc(mdev, dev_spec_op, mem_planar_dev_spec_op_cmyk4);
        } else
            set_dev_proc(mdev, copy_color, mem_planar_copy_color);
        set_dev_proc(mdev, copy_alpha, gx_default_copy_alpha);
        set_dev_proc(mdev, copy_plane, mem_planar_copy_plane);
        set_dev_proc(mdev, strip_tile_rectangle, mem_planar_strip_tile_rectangle);
        set_dev_proc(mdev, strip_copy_rop, mem_planar_strip_copy_rop);
        set_dev_proc(mdev, get_bits_rectangle, mem_planar_get_bits_rectangle);
    }
    return 0;
}

/* Open a planar memory device. */
static int
mem_planar_open(gx_device * dev)
{
    gx_device_memory *const mdev = (gx_device_memory *)dev;

    /* Check that we aren't trying to open a chunky device as planar. */
    if (mdev->num_planes == 0)
        return_error(gs_error_rangecheck);
    return gdev_mem_open_scan_lines(mdev, dev->height);
}

/*
 * We execute drawing operations by patching a few parameters in the
 * device structure and then calling the procedure appropriate to the
 * plane depth.
 */
typedef struct mem_save_params_s {
    int depth;                  /* color_info.depth */
    byte *base;
    byte **line_ptrs;
} mem_save_params_t;
#define MEM_SAVE_PARAMS(mdev, msp)\
  (msp.depth = mdev->color_info.depth,\
   msp.base = mdev->base,\
   msp.line_ptrs = mdev->line_ptrs)
/* Previous versions of MEM_SET_PARAMS calculated raster as
 * bitmap_raster(mdev->width * plane_depth), but this restricts us to
 * non interleaved frame buffers. Now we calculate it from the difference
 * between the first 2 line pointers; this clearly only works if there are
 * at least 2 line pointers to use. Otherwise, we fall back to the old
 * method.
 */
/* FIXME: Find a nicer way of calculating raster. */
#define MEM_SET_PARAMS(mdev, plane_depth)\
  (mdev->color_info.depth = plane_depth, /* maybe not needed */\
   mdev->base = mdev->line_ptrs[0],\
   mdev->raster = (mdev->height > 1 ? mdev->line_ptrs[1]-mdev->line_ptrs[0] : bitmap_raster(mdev->width * plane_depth)))
#define MEM_RESTORE_PARAMS(mdev, msp)\
  (mdev->color_info.depth = msp.depth,\
   mdev->base = msp.base,\
   mdev->line_ptrs = msp.line_ptrs)

/* Fill a rectangle with a color. */
static int
mem_planar_fill_rectangle(gx_device * dev, int x, int y, int w, int h,
                          gx_color_index color)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
    mem_save_params_t save;
    int pi;

    MEM_SAVE_PARAMS(mdev, save);
    for (pi = 0; pi < mdev->num_planes; ++pi) {
        int plane_depth = mdev->planes[pi].depth;
        gx_color_index mask = ((gx_color_index)1 << plane_depth) - 1;
        const gx_device_memory *mdproto =
            gdev_mem_device_for_bits(plane_depth);

        MEM_SET_PARAMS(mdev, plane_depth);
        dev_proc(mdproto, fill_rectangle)(dev, x, y, w, h,
                                          (color >> mdev->planes[pi].shift) &
                                          mask);
        mdev->line_ptrs += mdev->height;
    }
    MEM_RESTORE_PARAMS(mdev, save);
    return 0;
}

/* Copy a bitmap. */
static int
mem_planar_copy_mono(gx_device * dev, const byte * base, int sourcex,
                     int sraster, gx_bitmap_id id, int x, int y, int w, int h,
                     gx_color_index color0, gx_color_index color1)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
    mem_save_params_t save;
    int pi;

    MEM_SAVE_PARAMS(mdev, save);
    for (pi = 0; pi < mdev->num_planes; ++pi) {
        int plane_depth = mdev->planes[pi].depth;
        int shift = mdev->planes[pi].shift;
        gx_color_index mask = ((gx_color_index)1 << plane_depth) - 1;
        const gx_device_memory *mdproto =
            gdev_mem_device_for_bits(plane_depth);
        gx_color_index c0 =
            (color0 == gx_no_color_index ? gx_no_color_index :
             (color0 >> shift) & mask);
        gx_color_index c1 =
            (color1 == gx_no_color_index ? gx_no_color_index :
             (color1 >> shift) & mask);

        MEM_SET_PARAMS(mdev, plane_depth);
        if (c0 == c1)
            dev_proc(mdproto, fill_rectangle)(dev, x, y, w, h, c0);
        else
            dev_proc(mdproto, copy_mono)
                (dev, base, sourcex, sraster, id, x, y, w, h, c0, c1);
        mdev->line_ptrs += mdev->height;
    }
    MEM_RESTORE_PARAMS(mdev, save);
    return 0;
}

/* Copy color: Special case the 24 -> 8+8+8 case. */
static int
mem_planar_copy_color_24to8(gx_device * dev, const byte * base, int sourcex,
                            int sraster, gx_bitmap_id id,
                            int x, int y, int w, int h)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
#define BUF_LONGS 100   /* arbitrary, >= 1 */
#define BUF_BYTES (BUF_LONGS * ARCH_SIZEOF_LONG)
    union b_ {
        ulong l[BUF_LONGS];
        byte b[BUF_BYTES];
    } buf, buf1, buf2;
    mem_save_params_t save;
    const gx_device_memory *mdproto = gdev_mem_device_for_bits(8);
    uint plane_raster = bitmap_raster(w<<3);
    int br, bw, bh, cx, cy, cw, ch, ix, iy;

    fit_copy(dev, base, sourcex, sraster, id, x, y, w, h);
    MEM_SAVE_PARAMS(mdev, save);
    MEM_SET_PARAMS(mdev, 8);
    if (plane_raster > BUF_BYTES) {
        br = BUF_BYTES;
        bw = BUF_BYTES;
        bh = 1;
    } else {
        br = plane_raster;
        bw = w;
        bh = BUF_BYTES / plane_raster;
    }
    for (cy = y; cy < y + h; cy += ch) {
        ch = min(bh, y + h - cy);
        for (cx = x; cx < x + w; cx += cw) {
            int sx = sourcex + cx - x;
            const byte *source_base = base + sraster * (cy - y);

            cw = min(bw, x + w - cx);
            source_base += sx * 3;
            for (iy = 0; iy < ch; ++iy) {
                const byte *sptr = source_base;
                byte *dptr0 = buf.b  + br * iy;
                byte *dptr1 = buf1.b + br * iy;
                byte *dptr2 = buf2.b + br * iy;
                ix = cw;
                do {
                    /* Use the temporary variables below to free the C compiler
                     * to interleave load/stores for latencies sake despite the
                     * pointer aliasing rules. */
                    byte r = *sptr++;
                    byte g = *sptr++;
                    byte b = *sptr++;
                    *dptr0++ = r;
                    *dptr1++ = g;
                    *dptr2++ = b;
                } while (--ix);
                source_base += sraster;
            }
            dev_proc(mdproto, copy_color)
                        (dev, buf.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch);
            mdev->line_ptrs += mdev->height;
            dev_proc(mdproto, copy_color)
                    (dev, buf1.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch);
            mdev->line_ptrs += mdev->height;
            dev_proc(mdproto, copy_color)
                    (dev, buf2.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch);
            mdev->line_ptrs -= 2*mdev->height;
        }
    }
    MEM_RESTORE_PARAMS(mdev, save);
    return 0;
}

/* Copy color: Special case the 4 -> 1+1+1+1 case. */
/* Two versions of this routine; the first does bit comparisons. This should
 * work well on architectures with small cache and conditional execution
 * (such as ARM). Hurts on x86 due to the ifs in the loop all causing small
 * skips ahead that defeat the branch predictor.
 * Second version uses a table lookup; 1K of table is nothing on x86, and
 * so this runs much faster. */
#ifdef PREFER_ALTERNATIION_TO_TABLES
static int
mem_planar_copy_color_4to1(gx_device * dev, const byte * base, int sourcex,
                            int sraster, gx_bitmap_id id,
                            int x, int y, int w, int h)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
#define BUF_LONGS 100   /* arbitrary, >= 1 */
#define BUF_BYTES (BUF_LONGS * ARCH_SIZEOF_LONG)
    union b_ {
        ulong l[BUF_LONGS];
        byte b[BUF_BYTES];
    } buf0, buf1, buf2, buf3;
    mem_save_params_t save;
    const gx_device_memory *mdproto = gdev_mem_device_for_bits(1);
    uint plane_raster = bitmap_raster(w);
    int br, bw, bh, cx, cy, cw, ch, ix, iy;

    fit_copy(dev, base, sourcex, sraster, id, x, y, w, h);
    MEM_SAVE_PARAMS(mdev, save);
    MEM_SET_PARAMS(mdev, 1);
    if (plane_raster > BUF_BYTES) {
        br = BUF_BYTES;
        bw = BUF_BYTES<<3;
        bh = 1;
    } else {
        br = plane_raster;
        bw = w;
        bh = BUF_BYTES / plane_raster;
    }
    for (cy = y; cy < y + h; cy += ch) {
        ch = min(bh, y + h - cy);
        for (cx = x; cx < x + w; cx += cw) {
            int sx = sourcex + cx - x;
            const byte *source_base = base + sraster * (cy - y) + (sx>>1);

            cw = min(bw, x + w - cx);
            if ((sx & 1) == 0) {
                for (iy = 0; iy < ch; ++iy) {
                    const byte *sptr = source_base;
                    byte *dptr0 = buf0.b + br * iy;
                    byte *dptr1 = buf1.b + br * iy;
                    byte *dptr2 = buf2.b + br * iy;
                    byte *dptr3 = buf3.b + br * iy;
                    byte roll = 0x80;
                    byte bc = 0;
                    byte bm = 0;
                    byte by = 0;
                    byte bk = 0;
                    ix = cw;
                    do {
                        byte b = *sptr++;
                        if (b & 0x80)
                            bc |= roll;
                        if (b & 0x40)
                            bm |= roll;
                        if (b & 0x20)
                            by |= roll;
                        if (b & 0x10)
                            bk |= roll;
                        roll >>= 1;
                        if (b & 0x08)
                            bc |= roll;
                        if (b & 0x04)
                            bm |= roll;
                        if (b & 0x02)
                            by |= roll;
                        if (b & 0x01)
                            bk |= roll;
                        roll >>= 1;
                        if (roll == 0) {
                            *dptr0++ = bc;
                            *dptr1++ = bm;
                            *dptr2++ = by;
                            *dptr3++ = bk;
                            bc = 0;
                            bm = 0;
                            by = 0;
                            bk = 0;
                            roll = 0x80;
                        }
                        ix -= 2;
                    } while (ix > 0);
                    if (roll != 0x80) {
                        *dptr0++ = bc;
                        *dptr1++ = bm;
                        *dptr2++ = by;
                        *dptr3++ = bk;
                    }
                    source_base += sraster;
                }
            } else {
                for (iy = 0; iy < ch; ++iy) {
                    const byte *sptr = source_base;
                    byte *dptr0 = buf0.b + br * iy;
                    byte *dptr1 = buf1.b + br * iy;
                    byte *dptr2 = buf2.b + br * iy;
                    byte *dptr3 = buf3.b + br * iy;
                    byte roll = 0x80;
                    byte bc = 0;
                    byte bm = 0;
                    byte by = 0;
                    byte bk = 0;
                    byte b = *sptr++;
                    ix = cw;
                    goto loop_entry;
                    do {
                        b = *sptr++;
                        if (b & 0x80)
                            bc |= roll;
                        if (b & 0x40)
                            bm |= roll;
                        if (b & 0x20)
                            by |= roll;
                        if (b & 0x10)
                            bk |= roll;
                        roll >>= 1;
                        if (roll == 0) {
                            *dptr0++ = bc;
                            *dptr1++ = bm;
                            *dptr2++ = by;
                            *dptr3++ = bk;
                            bc = 0;
                            bm = 0;
                            by = 0;
                            bk = 0;
                            roll = 0x80;
                        }
loop_entry:
                        if (b & 0x08)
                            bc |= roll;
                        if (b & 0x04)
                            bm |= roll;
                        if (b & 0x02)
                            by |= roll;
                        if (b & 0x01)
                            bk |= roll;
                        roll >>= 1;
                        ix -= 2;
                    } while (ix >= 0); /* ix == -2 means 1 extra done */
                    if ((ix == -2) && (roll == 0x40)) {
                        /* We did an extra one, and it was the last thing
                         * we did. Nothing to store. */
                    } else {
                        /* Flush the stored bytes */
                        *dptr0++ = bc;
                        *dptr1++ = bm;
                        *dptr2++ = by;
                        *dptr3++ = bk;
                    }
                    source_base += sraster;
                }
            }
            dev_proc(mdproto, copy_mono)
                        (dev, buf0.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
            mdev->line_ptrs += mdev->height;
            dev_proc(mdproto, copy_mono)
                        (dev, buf1.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
            mdev->line_ptrs += mdev->height;
            dev_proc(mdproto, copy_mono)
                        (dev, buf2.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
            mdev->line_ptrs += mdev->height;
            dev_proc(mdproto, copy_mono)
                        (dev, buf3.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
            mdev->line_ptrs -= 3*mdev->height;
        }
    }
    MEM_RESTORE_PARAMS(mdev, save);
    return 0;
}
#else

static bits32 expand_4to1[256] =
{
0x00000000,0x00000001,0x00000100,0x00000101,
0x00010000,0x00010001,0x00010100,0x00010101,
0x01000000,0x01000001,0x01000100,0x01000101,
0x01010000,0x01010001,0x01010100,0x01010101,
0x00000002,0x00000003,0x00000102,0x00000103,
0x00010002,0x00010003,0x00010102,0x00010103,
0x01000002,0x01000003,0x01000102,0x01000103,
0x01010002,0x01010003,0x01010102,0x01010103,
0x00000200,0x00000201,0x00000300,0x00000301,
0x00010200,0x00010201,0x00010300,0x00010301,
0x01000200,0x01000201,0x01000300,0x01000301,
0x01010200,0x01010201,0x01010300,0x01010301,
0x00000202,0x00000203,0x00000302,0x00000303,
0x00010202,0x00010203,0x00010302,0x00010303,
0x01000202,0x01000203,0x01000302,0x01000303,
0x01010202,0x01010203,0x01010302,0x01010303,
0x00020000,0x00020001,0x00020100,0x00020101,
0x00030000,0x00030001,0x00030100,0x00030101,
0x01020000,0x01020001,0x01020100,0x01020101,
0x01030000,0x01030001,0x01030100,0x01030101,
0x00020002,0x00020003,0x00020102,0x00020103,
0x00030002,0x00030003,0x00030102,0x00030103,
0x01020002,0x01020003,0x01020102,0x01020103,
0x01030002,0x01030003,0x01030102,0x01030103,
0x00020200,0x00020201,0x00020300,0x00020301,
0x00030200,0x00030201,0x00030300,0x00030301,
0x01020200,0x01020201,0x01020300,0x01020301,
0x01030200,0x01030201,0x01030300,0x01030301,
0x00020202,0x00020203,0x00020302,0x00020303,
0x00030202,0x00030203,0x00030302,0x00030303,
0x01020202,0x01020203,0x01020302,0x01020303,
0x01030202,0x01030203,0x01030302,0x01030303,
0x02000000,0x02000001,0x02000100,0x02000101,
0x02010000,0x02010001,0x02010100,0x02010101,
0x03000000,0x03000001,0x03000100,0x03000101,
0x03010000,0x03010001,0x03010100,0x03010101,
0x02000002,0x02000003,0x02000102,0x02000103,
0x02010002,0x02010003,0x02010102,0x02010103,
0x03000002,0x03000003,0x03000102,0x03000103,
0x03010002,0x03010003,0x03010102,0x03010103,
0x02000200,0x02000201,0x02000300,0x02000301,
0x02010200,0x02010201,0x02010300,0x02010301,
0x03000200,0x03000201,0x03000300,0x03000301,
0x03010200,0x03010201,0x03010300,0x03010301,
0x02000202,0x02000203,0x02000302,0x02000303,
0x02010202,0x02010203,0x02010302,0x02010303,
0x03000202,0x03000203,0x03000302,0x03000303,
0x03010202,0x03010203,0x03010302,0x03010303,
0x02020000,0x02020001,0x02020100,0x02020101,
0x02030000,0x02030001,0x02030100,0x02030101,
0x03020000,0x03020001,0x03020100,0x03020101,
0x03030000,0x03030001,0x03030100,0x03030101,
0x02020002,0x02020003,0x02020102,0x02020103,
0x02030002,0x02030003,0x02030102,0x02030103,
0x03020002,0x03020003,0x03020102,0x03020103,
0x03030002,0x03030003,0x03030102,0x03030103,
0x02020200,0x02020201,0x02020300,0x02020301,
0x02030200,0x02030201,0x02030300,0x02030301,
0x03020200,0x03020201,0x03020300,0x03020301,
0x03030200,0x03030201,0x03030300,0x03030301,
0x02020202,0x02020203,0x02020302,0x02020303,
0x02030202,0x02030203,0x02030302,0x02030303,
0x03020202,0x03020203,0x03020302,0x03020303,
0x03030202,0x03030203,0x03030302,0x03030303
};

static int
mem_planar_copy_color_4to1(gx_device * dev, const byte * base, int sourcex,
                            int sraster, gx_bitmap_id id,
                            int x, int y, int w, int h)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
#define BUF_LONGS 100   /* arbitrary, >= 1 */
#define BUF_BYTES (BUF_LONGS * ARCH_SIZEOF_LONG)
    union b_ {
        ulong l[BUF_LONGS];
        byte b[BUF_BYTES];
    } buf0, buf1, buf2, buf3;
    mem_save_params_t save;
    const gx_device_memory *mdproto = gdev_mem_device_for_bits(1);
    uint plane_raster = bitmap_raster(w);
    int br, bw, bh, cx, cy, cw, ch, ix, iy;

    fit_copy(dev, base, sourcex, sraster, id, x, y, w, h);
    MEM_SAVE_PARAMS(mdev, save);
    MEM_SET_PARAMS(mdev, 1);
    if (plane_raster > BUF_BYTES) {
        br = BUF_BYTES;
        bw = BUF_BYTES<<3;
        bh = 1;
    } else {
        br = plane_raster;
        bw = w;
        bh = BUF_BYTES / plane_raster;
    }
    for (cy = y; cy < y + h; cy += ch) {
        ch = min(bh, y + h - cy);
        for (cx = x; cx < x + w; cx += cw) {
            int sx = sourcex + cx - x;
            const byte *source_base = base + sraster * (cy - y) + (sx>>1);

            cw = min(bw, x + w - cx);
            if ((sx & 1) == 0) {
                for (iy = 0; iy < ch; ++iy) {
                    const byte *sptr = source_base;
                    byte *dptr0 = buf0.b + br * iy;
                    byte *dptr1 = buf1.b + br * iy;
                    byte *dptr2 = buf2.b + br * iy;
                    byte *dptr3 = buf3.b + br * iy;
                    int roll = 6;
                    int cmyk = 0;
                    ix = cw;
                    do {
                        cmyk |= expand_4to1[*sptr++]<<roll;
                        roll -= 2;
                        if (roll < 0) {
                            *dptr0++ = cmyk>>24;
                            *dptr1++ = cmyk>>16;
                            *dptr2++ = cmyk>>8;
                            *dptr3++ = cmyk;
                            cmyk = 0;
                            roll = 6;
                        }
                        ix -= 2;
                    } while (ix > 0);
                    if (roll != 6) {
                        *dptr0++ = cmyk>>24;
                        *dptr1++ = cmyk>>16;
                        *dptr2++ = cmyk>>8;
                        *dptr3++ = cmyk;
                    }
                    source_base += sraster;
                }
            } else {
                for (iy = 0; iy < ch; ++iy) {
                    const byte *sptr = source_base;
                    byte *dptr0 = buf0.b + br * iy;
                    byte *dptr1 = buf1.b + br * iy;
                    byte *dptr2 = buf2.b + br * iy;
                    byte *dptr3 = buf3.b + br * iy;
                    int roll = 7;
                    int cmyk = 0;
                    byte b = *sptr++ & 0x0f;
                    ix = cw;
                    goto loop_entry;
                    do {
                        b = *sptr++;
                        roll -= 2;
                        if (roll < 0)
                        {
                            cmyk |= expand_4to1[b & 0xf0]>>1;
                            *dptr0++ = cmyk>>24;
                            *dptr1++ = cmyk>>16;
                            *dptr2++ = cmyk>>8;
                            *dptr3++ = cmyk;
                            cmyk = 0;
                            roll = 7;
                            b &= 0x0f;
                        }
loop_entry:
                        cmyk |= expand_4to1[b]<<roll;
                        ix -= 2;
                    } while (ix >= 0); /* ix == -2 means 1 extra done */
                    if ((ix == -2) && (roll == 7)) {
                        /* We did an extra one, and it was the last thing
                         * we did. Nothing to store. */
                    } else {
                        /* Flush the stored bytes */
                        *dptr0++ = cmyk>>24;
                        *dptr1++ = cmyk>>16;
                        *dptr2++ = cmyk>>8;
                        *dptr3++ = cmyk;
                    }
                    source_base += sraster;
                }
            }
            dev_proc(mdproto, copy_mono)
                        (dev, buf0.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
            mdev->line_ptrs += mdev->height;
            dev_proc(mdproto, copy_mono)
                        (dev, buf1.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
            mdev->line_ptrs += mdev->height;
            dev_proc(mdproto, copy_mono)
                        (dev, buf2.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
            mdev->line_ptrs += mdev->height;
            dev_proc(mdproto, copy_mono)
                        (dev, buf3.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
            mdev->line_ptrs -= 3*mdev->height;
        }
    }
    MEM_RESTORE_PARAMS(mdev, save);
    return 0;
}
#endif

/* Copy a color bitmap. */
/* This is slow and messy. */
static int
mem_planar_copy_color(gx_device * dev, const byte * base, int sourcex,
                      int sraster, gx_bitmap_id id,
                      int x, int y, int w, int h)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
#define BUF_LONGS 100   /* arbitrary, >= 1 */
#define BUF_BYTES (BUF_LONGS * ARCH_SIZEOF_LONG)
    union b_ {
        ulong l[BUF_LONGS];
        byte b[BUF_BYTES];
    } buf;
    int source_depth = dev->color_info.depth;
    mem_save_params_t save;
    int pi;

    fit_copy(dev, base, sourcex, sraster, id, x, y, w, h);
    MEM_SAVE_PARAMS(mdev, save);
    for (pi = 0; pi < mdev->num_planes; ++pi) {
        int plane_depth = mdev->planes[pi].depth;
        int shift = mdev->planes[pi].shift;
        gx_color_index mask = ((gx_color_index)1 << plane_depth) - 1;
        const gx_device_memory *mdproto =
            gdev_mem_device_for_bits(plane_depth);
        /*
         * Divide up the transfer into chunks that can be assembled
         * within the fixed-size buffer.  This code can be simplified
         * a lot if all planes have the same depth, by simply using
         * copy_color to transfer one column at a time, but it might
         * be very inefficient.
         */
        uint plane_raster = bitmap_raster(plane_depth * w);
        int br, bw, bh, cx, cy, cw, ch, ix, iy;

        MEM_SET_PARAMS(mdev, plane_depth);
        if (plane_raster > BUF_BYTES) {
            br = BUF_BYTES;
            bw = BUF_BYTES * 8 / plane_depth;
            bh = 1;
        } else {
            br = plane_raster;
            bw = w;
            bh = BUF_BYTES / plane_raster;
        }
        /*
         * We could do the extraction with get_bits_rectangle
         * selecting a single plane, but this is critical enough
         * code that we more or less replicate it here.
         */
        for (cy = y; cy < y + h; cy += ch) {
            ch = min(bh, y + h - cy);
            for (cx = x; cx < x + w; cx += cw) {
                int sx = sourcex + cx - x;
                const byte *source_base = base + sraster * (cy - y);
                int source_bit = 0;

                cw = min(bw, x + w - cx);
                if (sx) {
                    int xbit = sx * source_depth;

                    source_base += xbit >> 3;
                    source_bit = xbit & 7;
                }
                for (iy = 0; iy < ch; ++iy) {
                    sample_load_declare_setup(sptr, sbit, source_base,
                                              source_bit, source_depth);
                    sample_store_declare_setup(dptr, dbit, dbbyte,
                                               buf.b + br * iy,
                                               0, plane_depth);

                    for (ix = 0; ix < cw; ++ix) {
                        gx_color_index value;

                        sample_load_next_any(value, sptr, sbit, source_depth);
                        value = (value >> shift) & mask;
                        sample_store_next16(value, dptr, dbit, plane_depth,
                                            dbbyte);
                    }
                    sample_store_flush(dptr, dbit, plane_depth, dbbyte);
                    source_base += sraster;
                }
                /*
                 * Detect and bypass the possibility that copy_color is
                 * defined in terms of copy_mono.
                 */
                if (plane_depth == 1)
                    dev_proc(mdproto, copy_mono)
                        (dev, buf.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch,
                         (gx_color_index)0, (gx_color_index)1);
                else
                    dev_proc(mdproto, copy_color)
                        (dev, buf.b, 0, br, gx_no_bitmap_id, cx, cy, cw, ch);
            }
        }
        mdev->line_ptrs += mdev->height;
    }
    MEM_RESTORE_PARAMS(mdev, save);
    return 0;
#undef BUF_BYTES
#undef BUF_LONGS
}

/* Copy a given bitmap into a bitmap. */
static int
mem_planar_copy_plane(gx_device * dev, const byte * base, int sourcex,
                      int sraster, gx_bitmap_id id,
                      int x, int y, int w, int h, int plane)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
    int plane_depth;
    mem_save_params_t save;
    const gx_device_memory *mdproto;
    int code;

    if ((plane < 0) || (plane >= mdev->num_planes))
        return gs_error_rangecheck;
    MEM_SAVE_PARAMS(mdev, save);
    mdev->line_ptrs += mdev->height * plane;
    plane_depth = mdev->planes[plane].depth;
    mdproto = gdev_mem_device_for_bits(plane_depth);
    if (plane_depth == 1)
        code = dev_proc(mdproto, copy_mono)(dev, base, sourcex, sraster, id,
                                            x, y, w, h,
                                            (gx_color_index)0,
                                            (gx_color_index)1);
    else
        code = dev_proc(mdproto, copy_color)(dev, base, sourcex, sraster,
                                             id, x, y, w, h);
    MEM_RESTORE_PARAMS(mdev, save);
    return code;
}

static int
mem_planar_strip_tile_rectangle(gx_device * dev, const gx_strip_bitmap * tiles,
                                int x, int y, int w, int h,
                                gx_color_index color0, gx_color_index color1,
                                int px, int py)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
    mem_save_params_t save;
    int pi;

    /* We can't split up the transfer if the tile is colored. */
    if (color0 == gx_no_color_index && color1 == gx_no_color_index)
        return gx_default_strip_tile_rectangle
            (dev, tiles, x, y, w, h, color0, color1, px, py);
    MEM_SAVE_PARAMS(mdev, save);
    for (pi = 0; pi < mdev->num_planes; ++pi) {
        int plane_depth = mdev->planes[pi].depth;
        int shift = mdev->planes[pi].shift;
        gx_color_index mask = ((gx_color_index)1 << plane_depth) - 1;
        const gx_device_memory *mdproto =
            gdev_mem_device_for_bits(plane_depth);
        gx_color_index c0 =
            (color0 == gx_no_color_index ? gx_no_color_index :
             (color0 >> shift) & mask);
        gx_color_index c1 =
            (color1 == gx_no_color_index ? gx_no_color_index :
             (color1 >> shift) & mask);

        MEM_SET_PARAMS(mdev, plane_depth);
        if (c0 == c1)
            dev_proc(mdproto, fill_rectangle)(dev, x, y, w, h, c0);
        else {
            /*
             * Temporarily replace copy_mono in case strip_tile_rectangle is
             * defined in terms of it.
             */
            set_dev_proc(dev, copy_mono, dev_proc(mdproto, copy_mono));
            dev_proc(mdproto, strip_tile_rectangle)
                (dev, tiles, x, y, w, h, c0, c1, px, py);
        }
        mdev->line_ptrs += mdev->height;
    }
    MEM_RESTORE_PARAMS(mdev, save);
    set_dev_proc(dev, copy_mono, mem_planar_copy_mono);
    return 0;
}

static int
planar_cmyk4bit_strip_copy_rop(gx_device_memory * mdev,
                               const byte * srow, int sourcex, uint sraster,
                               gx_bitmap_id id, const gx_color_index * scolors,
                               const gx_strip_bitmap * textures,
                               const gx_color_index * tcolors,
                               int x, int y, int width, int height,
                               int phase_x, int phase_y,
                               gs_logical_operation_t lop)
{
    gs_rop3_t rop = (gs_rop3_t)lop;
    uint draster = mdev->raster;
    int line_count;
    byte *cdrow, *mdrow, *ydrow, *kdrow;
    byte lmask, rmask;
    rop_proc cproc, mproc, yproc;
    int dbit;
    int cscolor, mscolor, yscolor, kscolor;
    int ctcolor, mtcolor, ytcolor, ktcolor;
    int constant_s = 0;

    /* Modify the raster operation according to the source palette. */
    fit_copy(mdev, srow, sourcex, sraster, id, x, y, width, height);

    /* This function assumes constant (or unused) scolors and tcolors */
    if (scolors)
    {
        if (scolors[0] == scolors[1]) {
            kscolor = ((scolors[0] & 1) ? -1 : 0);
            cscolor = ((scolors[0] & 8) ? -1 : 0) | kscolor;
            mscolor = ((scolors[0] & 4) ? -1 : 0) | kscolor;
            yscolor = ((scolors[0] & 2) ? -1 : 0) | kscolor;
            constant_s = 1;
        } else {
            kscolor =  (scolors[0] & 1)     | ((scolors[1] & 1)<<1);
            cscolor = ((scolors[0] & 8)>>3) | ((scolors[1] & 8)>>2) | kscolor;
            mscolor = ((scolors[0] & 4)>>2) | ((scolors[1] & 4)>>1) | kscolor;
            yscolor = ((scolors[0] & 2)>>1) |  (scolors[1] & 2)     | kscolor;
            switch (cscolor) {
                case 0:
                    cproc = rop_proc_table[rop3_know_S_0(rop)];
                    break;
                case 1:
                    cproc = rop_proc_table[rop3_invert_S(rop)];
                    break;
                case 2:
                    cproc = rop_proc_table[rop];
                    break;
                default: /* 3 */
                    cproc = rop_proc_table[rop3_know_S_1(rop)];
                    break;
            }
            switch (mscolor) {
                case 0:
                    mproc = rop_proc_table[rop3_know_S_0(rop)];
                    break;
                case 1:
                    mproc = rop_proc_table[rop3_invert_S(rop)];
                    break;
                case 2:
                    mproc = rop_proc_table[rop];
                    break;
                default: /* 3 */
                    mproc = rop_proc_table[rop3_know_S_1(rop)];
                    break;
            }
            switch (yscolor) {
                case 0:
                    yproc = rop_proc_table[rop3_know_S_0(rop)];
                    break;
                case 1:
                    yproc = rop_proc_table[rop3_invert_S(rop)];
                    break;
                case 2:
                    yproc = rop_proc_table[rop];
                    break;
                default: /* 3 */
                    yproc = rop_proc_table[rop3_know_S_1(rop)];
                    break;
            }
        }
    }
    if (tcolors)
    {
        ktcolor = ((tcolors[0] & 1) ? -1 : 0);
        ctcolor = ((tcolors[0] & 8) ? -1 : 0) | ktcolor;
        mtcolor = ((tcolors[0] & 4) ? -1 : 0) | ktcolor;
        ytcolor = ((tcolors[0] & 2) ? -1 : 0) | ktcolor;
    }

    /* Set up transfer parameters. */
    line_count = height;
    dbit = x & 7;
    cdrow = scan_line_base(mdev, y) + (x>>3);
    mdrow = cdrow + mdev->height * draster;
    ydrow = mdrow + mdev->height * draster;
    kdrow = ydrow + mdev->height * draster;

    lmask = 0xff >> dbit;
    width += dbit;
    rmask = 0xff << (~(width - 1) & 7);
    if (width < 8)
        lmask &= rmask;
    if (scolors == NULL) {
        /* sptr points to bytes of cmykcmyk. Need to convert these to
         * planar format. */
        const rop_proc proc = rop_proc_table[rop];
        int sbit = sourcex & 1;
        int sskew = sbit - dbit; /* -7 >= sskew >= 1 */
        srow += (sskew>>1); /* Backtrack srow if required. */
        srow += (sourcex>>1);
        for (; line_count-- > 0; cdrow += draster, mdrow += draster, ydrow += draster, kdrow += draster, srow += sraster) {
            byte *cdptr = cdrow;
            byte *mdptr = mdrow;
            byte *ydptr = ydrow;
            byte *kdptr = kdrow;
            const byte *sptr = srow;
            int left = width-8;
            {
                /* Left hand bytes */
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr;
                byte mdbyte = *mdptr;
                byte ydbyte = *ydptr;
                byte cresult, mresult, yresult, kresult;
                bits32 scol = 0;
                if ((sskew & 1) == 0) {
                    if (sskew >= 0)
                        scol = expand_4to1[sptr[0]]<<6;
                    if ((sskew >= -2) && (left > -6))
                        scol |= expand_4to1[sptr[1]]<<4;
                    if ((sskew >= -4) && (left > -4))
                        scol |= expand_4to1[sptr[2]]<<2;
                    if (left > -2)
                        scol |= expand_4to1[sptr[3]];
                } else {
                    if (sskew >= 0)
                        scol = expand_4to1[sptr[0] & 0x0f]<<7;
                    if ((sskew >= -2) && (left > -7))
                        scol |= expand_4to1[sptr[1]]<<5;
                    if ((sskew >= -4) && (left > -5))
                        scol |= expand_4to1[sptr[2]]<<3;
                    if ((sskew >= -6) && (left > -3))
                        scol |= expand_4to1[sptr[3]]<<2;
                    if (left > -1)
                        scol |= expand_4to1[sptr[4] & 0xf0]>>1;
                }
                cresult = (*proc)(cdbyte | kdbyte,scol|(scol>>24),ctcolor);
                mresult = (*proc)(mdbyte | kdbyte,scol|(scol>>16),mtcolor);
                yresult = (*proc)(ydbyte | kdbyte,scol|(scol>> 8),ytcolor);
                kresult = cresult & mresult & yresult;
                cresult &= ~kresult;
                mresult &= ~kresult;
                yresult &= ~kresult;
                *cdptr++ = (cresult & lmask) | (cdbyte & ~lmask);
                *mdptr++ = (mresult & lmask) | (mdbyte & ~lmask);
                *ydptr++ = (yresult & lmask) | (ydbyte & ~lmask);
                *kdptr++ = (kresult & lmask) | (kdbyte & ~lmask);
            }
            if (left <= 0) /* if (width <= 8) we're done */
                continue;
            left -= 8; /* left = bits to go - 8 */
            while (left > 0)
            {
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr | kdbyte;
                byte mdbyte = *mdptr | kdbyte;
                byte ydbyte = *ydptr | kdbyte;
                byte cresult, mresult, yresult, kresult;
                bits32 scol = 0;
                if ((sskew & 1) == 0) {
                    if (sskew >= 0)
                        scol = expand_4to1[sptr[0]]<<6;
                    if ((sskew >= -2) && (left > -6))
                        scol |= expand_4to1[sptr[1]]<<4;
                    if ((sskew >= -4) && (left > -4))
                        scol |= expand_4to1[sptr[2]]<<2;
                    if (left > -2)
                        scol |= expand_4to1[sptr[3]];
                } else {
                    if (sskew >= 0)
                        scol = expand_4to1[sptr[0] & 0x0f]<<7;
                    if ((sskew >= -2) && (left > -7))
                        scol |= expand_4to1[sptr[1]]<<5;
                    if ((sskew >= -4) && (left > -5))
                        scol |= expand_4to1[sptr[2]]<<3;
                    if ((sskew >= -6) && (left > -3))
                        scol |= expand_4to1[sptr[3]]<<2;
                    if (left > -1)
                        scol |= expand_4to1[sptr[4] & 0xf0]>>1;
                }
                cresult = (*proc)(cdbyte | kdbyte,scol|(scol>>24),ctcolor);
                mresult = (*proc)(mdbyte | kdbyte,scol|(scol>>16),mtcolor);
                yresult = (*proc)(ydbyte | kdbyte,scol|(scol>> 8),ytcolor);
                kresult = cresult & mresult & yresult;
                cresult &= ~kresult;
                mresult &= ~kresult;
                yresult &= ~kresult;
                *cdptr++ = cresult & ~kresult;
                *mdptr++ = mresult & ~kresult;
                *ydptr++ = yresult & ~kresult;
                *kdptr++ = kresult;
                left -= 8;
            }
            left += 8; /* left = bits to go < 8 */
            {
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr;
                byte mdbyte = *mdptr;
                byte ydbyte = *ydptr;
                byte cresult, mresult, yresult, kresult;
                bits32 scol = 0;
                if ((sskew & 1) == 0) {
                    if (sskew >= 0)
                        scol = expand_4to1[sptr[0]]<<6;
                    if ((sskew >= -2) && (left > -6))
                        scol |= expand_4to1[sptr[1]]<<4;
                    if ((sskew >= -4) && (left > -4))
                        scol |= expand_4to1[sptr[2]]<<2;
                    if (left > -2)
                        scol |= expand_4to1[sptr[3]];
                } else {
                    if (sskew >= 0)
                        scol = expand_4to1[sptr[0] & 0x0f]<<7;
                    if ((sskew >= -2) && (left > -7))
                        scol |= expand_4to1[sptr[1]]<<5;
                    if ((sskew >= -4) && (left > -5))
                        scol |= expand_4to1[sptr[2]]<<3;
                    if ((sskew >= -6) && (left > -3))
                        scol |= expand_4to1[sptr[3]]<<2;
                    if (left > -1)
                        scol |= expand_4to1[sptr[4] & 0xf0]>>1;
                }
                cresult = (*proc)(cdbyte | kdbyte,scol|(scol>>24),ctcolor);
                mresult = (*proc)(mdbyte | kdbyte,scol|(scol>>16),mtcolor);
                yresult = (*proc)(ydbyte | kdbyte,scol|(scol>> 8),ytcolor);
                kresult = cresult & mresult & yresult;
                cresult &= ~kresult;
                mresult &= ~kresult;
                yresult &= ~kresult;
                *cdptr++ = (cresult & rmask) | (cdbyte & ~rmask);
                *mdptr++ = (mresult & rmask) | (mdbyte & ~rmask);
                *ydptr++ = (yresult & rmask) | (ydbyte & ~rmask);
                *kdptr++ = (kresult & rmask) | (kdbyte & ~rmask);
            }
        }
    } else if (constant_s) {
        const rop_proc proc = rop_proc_table[rop];
        int sbit = sourcex & 7;
        srow += (sourcex>>3);
        for (; line_count-- > 0; cdrow += draster, mdrow += draster, ydrow += draster, kdrow += draster) {
            byte *cdptr = cdrow;
            byte *mdptr = mdrow;
            byte *ydptr = ydrow;
            byte *kdptr = kdrow;
            int left = width-8;
            {
                /* Left hand bytes */
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr;
                byte mdbyte = *mdptr;
                byte ydbyte = *ydptr;
                byte cresult = (*proc)(cdbyte | kdbyte,cscolor,ctcolor);
                byte mresult = (*proc)(mdbyte | kdbyte,mscolor,mtcolor);
                byte yresult = (*proc)(ydbyte | kdbyte,yscolor,ytcolor);
                byte kresult = cresult & mresult & yresult;
                cresult &= ~kresult;
                mresult &= ~kresult;
                yresult &= ~kresult;
                *cdptr++ = (cresult & lmask) | (cdbyte & ~lmask);
                *mdptr++ = (mresult & lmask) | (mdbyte & ~lmask);
                *ydptr++ = (yresult & lmask) | (ydbyte & ~lmask);
                *kdptr++ = (kresult & lmask) | (kdbyte & ~lmask);
            }
            if (left <= 0) /* if (width <= 8) we're done */
                continue;
            left -= 8; /* left = bits to go - 8 */
            while (left > 0)
            {
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr | kdbyte;
                byte mdbyte = *mdptr | kdbyte;
                byte ydbyte = *ydptr | kdbyte;
                byte cresult = (*proc)(cdbyte,cscolor,ctcolor);
                byte mresult = (*proc)(mdbyte,mscolor,mtcolor);
                byte yresult = (*proc)(ydbyte,yscolor,ytcolor);
                byte kresult = cresult & mresult & yresult;
                cresult &= ~kresult;
                mresult &= ~kresult;
                yresult &= ~kresult;
                *cdptr++ = cresult & ~kresult;
                *mdptr++ = mresult & ~kresult;
                *ydptr++ = yresult & ~kresult;
                *kdptr++ = kresult;
                left -= 8;
            }
            left += 8; /* left = bits to go < 8 */
            {
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr;
                byte mdbyte = *mdptr;
                byte ydbyte = *ydptr;
                byte cresult = (*proc)(cdbyte | kdbyte,cscolor,ctcolor);
                byte mresult = (*proc)(mdbyte | kdbyte,mscolor,mtcolor);
                byte yresult = (*proc)(ydbyte | kdbyte,yscolor,ytcolor);
                byte kresult = cresult & mresult & yresult;
                cresult &= ~kresult;
                mresult &= ~kresult;
                yresult &= ~kresult;
                *cdptr++ = (cresult & rmask) | (cdbyte & ~rmask);
                *mdptr++ = (mresult & rmask) | (mdbyte & ~rmask);
                *ydptr++ = (yresult & rmask) | (ydbyte & ~rmask);
                *kdptr++ = (kresult & rmask) | (kdbyte & ~rmask);
            }
        }
    } else {
        /* Constant T, bitmap S */
        int sbit = sourcex & 7;
        int sskew = sbit - dbit;
        if (sskew < 0)
            --srow, sskew += 8;
        srow += (sourcex>>3);
        for (; line_count-- > 0; cdrow += draster, mdrow += draster, ydrow += draster, kdrow += draster, srow += sraster) {
            const byte *sptr = srow;
            byte *cdptr = cdrow;
            byte *mdptr = mdrow;
            byte *ydptr = ydrow;
            byte *kdptr = kdrow;
            int left = width-8;
            {
                /* Left hand byte (maybe the only one) */
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr;
                byte mdbyte = *mdptr;
                byte ydbyte = *ydptr;
#define fetch1(ptr, skew)\
  (skew ? (ptr[0] << skew) + (ptr[1] >> (8 - skew)) : *ptr)
                byte sbyte = fetch1(sptr, sskew);
                byte cresult = (*cproc)(cdbyte|kdbyte,sbyte,ctcolor);
                byte mresult = (*mproc)(mdbyte|kdbyte,sbyte,mtcolor);
                byte yresult = (*yproc)(ydbyte|kdbyte,sbyte,ytcolor);
                byte kresult = cresult & mresult & yresult;
                cresult &= ~kresult;
                mresult &= ~kresult;
                yresult &= ~kresult;
                *cdptr++ = (cresult & lmask) | (cdbyte & ~lmask);
                *mdptr++ = (mresult & lmask) | (mdbyte & ~lmask);
                *ydptr++ = (yresult & lmask) | (ydbyte & ~lmask);
                *kdptr++ = (kresult & lmask) | (kdbyte & ~lmask);
                sptr++;
                left -= 8;
            }
            while (left > 0) {
                /* Bytes where all 8 bits of S are needed */
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr | kdbyte;
                byte mdbyte = *mdptr | kdbyte;
                byte ydbyte = *ydptr | kdbyte;
                byte sbyte = fetch1(sptr, sskew);
                byte cresult = (*cproc)(cdbyte,sbyte,ctcolor);
                byte mresult = (*mproc)(mdbyte,sbyte,mtcolor);
                byte yresult = (*yproc)(ydbyte,sbyte,ytcolor);
                byte kresult = cresult & mresult & yresult;
                *cdptr++ = cresult & ~kresult;
                *mdptr++ = mresult & ~kresult;
                *ydptr++ = yresult & ~kresult;
                *kdptr++ = kresult;
                sptr++;
                left -= 8;
            }
            /* Final byte */
            if (left > -8) {
                byte kdbyte = *kdptr;
                byte cdbyte = *cdptr;
                byte mdbyte = *mdptr;
                byte ydbyte = *ydptr;
                byte sbyte = fetch1(sptr, sskew);
#undef fetch1
                byte cresult = (*cproc)(cdbyte | kdbyte,sbyte,ctcolor);
                byte mresult = (*mproc)(mdbyte | kdbyte,sbyte,mtcolor);
                byte yresult = (*yproc)(ydbyte | kdbyte,sbyte,ytcolor);
                byte kresult = cresult & mresult & yresult;
                cresult &= ~kresult;
                mresult &= ~kresult;
                yresult &= ~kresult;
                *cdptr++ = (cresult & rmask) | (cdbyte & ~rmask);
                *mdptr++ = (mresult & rmask) | (mdbyte & ~rmask);
                *ydptr++ = (yresult & rmask) | (ydbyte & ~rmask);
                *kdptr++ = (kresult & rmask) | (kdbyte & ~rmask);
            }
        }
    }
    return 0;
}

static int
plane_strip_copy_rop(gx_device_memory * mdev,
                     const byte * sdata, int sourcex, uint sraster,
                     gx_bitmap_id id, const gx_color_index * scolors,
                     const gx_strip_bitmap * textures,
                     const gx_color_index * tcolors,
                     int x, int y, int width, int height,
                     int phase_x, int phase_y,
                     gs_logical_operation_t lop, int plane)
{
    mem_save_params_t save;
    int code;
    const gx_device_memory *mdproto;

    MEM_SAVE_PARAMS(mdev, save);
    mdev->line_ptrs += mdev->height * plane;
    mdproto = gdev_mem_device_for_bits(mdev->planes[plane].depth);
    /* strip_copy_rop might end up calling get_bits_rectangle or fill_rectangle,
     * so ensure we have the right ones in there. */
    set_dev_proc(mdev, get_bits_rectangle, dev_proc(mdproto, get_bits_rectangle));
    set_dev_proc(mdev, fill_rectangle, dev_proc(mdproto, fill_rectangle));
    code = dev_proc(mdproto, strip_copy_rop)((gx_device *)mdev, sdata, sourcex, sraster,
                                             id, scolors, textures, tcolors,
                                             x, y, width, height,
                                             phase_x, phase_y, lop);
    set_dev_proc(mdev, get_bits_rectangle, mem_planar_get_bits_rectangle);
    set_dev_proc(mdev, fill_rectangle, mem_planar_fill_rectangle);
    /* The following effectively does: mdev->line_ptrs -= mdev->height * plane; */
    MEM_RESTORE_PARAMS(mdev, save);
    return code;
}

static byte cmykrop[256] =
{
    255,127,191,63,223,95,159,31,239,111,175,47,207,79,143,15,
    247,119,183,55,215,87,151,23,231,103,167,39,199,71,135,7,
    251,123,187,59,219,91,155,27,235,107,171,43,203,75,139,11,
    243,115,179,51,211,83,147,19,227,99,163,35,195,67,131,3,
    253,125,189,61,221,93,157,29,237,109,173,45,205,77,141,13,
    245,117,181,53,213,85,149,21,229,101,165,37,197,69,133,5,
    249,121,185,57,217,89,153,25,233,105,169,41,201,73,137,9,
    241,113,177,49,209,81,145,17,225,97,161,33,193,65,129,1,
    254,126,190,62,222,94,158,30,238,110,174,46,206,78,142,14,
    246,118,182,54,214,86,150,22,230,102,166,38,198,70,134,6,
    250,122,186,58,218,90,154,26,234,106,170,42,202,74,138,10,
    242,114,178,50,210,82,146,18,226,98,162,34,194,66,130,2,
    252,124,188,60,220,92,156,28,236,108,172,44,204,76,140,12,
    244,116,180,52,212,84,148,20,228,100,164,36,196,68,132,4,
    248,120,184,56,216,88,152,24,232,104,168,40,200,72,136,8,
    240,112,176,48,208,80,144,16,224,96,160,32,192,64,128,0
};

static int
mem_planar_strip_copy_rop(gx_device * dev,
                          const byte * sdata, int sourcex, uint sraster,
                          gx_bitmap_id id, const gx_color_index * scolors,
                          const gx_strip_bitmap * textures,
                          const gx_color_index * tcolors,
                          int x, int y, int width, int height,
                          int phase_x, int phase_y,
                          gs_logical_operation_t lop)
{
    gx_device_memory * const mdev = (gx_device_memory *)dev;
    int plane, code;

    if ((lop & lop_planar) == 0) {
        /* Not doing a planar lop. If we carry on down the default path here,
         * we'll end up doing a planar_to_chunky; we may be able to sidestep
         * that by spotting cases where we can operate directly. */
        if (!lop_uses_T(lop) || (tcolors && (tcolors[0] == tcolors[1]))) {
            /* No T in use, or constant T. */
            if ((!lop_uses_S(lop) || (scolors && (scolors[0] == scolors[1]))) &&
                ((mdev->num_planes == 1) || (mdev->num_planes == 3))) {
                /* No S in use, or constant S. And either greyscale or rgb,
                 * so we can just do the rop on each plane in turn. */
                for (plane=0; plane < mdev->num_planes; plane++)
                {
                    gx_color_index tcolors2[2], scolors2[2];
                    int shift = mdev->planes[plane].shift;
                    int mask = (1<<mdev->planes[plane].depth)-1;

                    if (tcolors) {
                        tcolors2[0] = (tcolors[0] >> shift) & mask;
                        tcolors2[1] = (tcolors[1] >> shift) & mask;
                    }
                    if (scolors) {
                        scolors2[0] = (scolors[0] >> shift) & mask;
                        scolors2[1] = (scolors[1] >> shift) & mask;
                    }
                    code = plane_strip_copy_rop(mdev, sdata, sourcex, sraster,
                                                id, (scolors ? scolors2 : NULL),
                                                textures, (tcolors ? tcolors2 : NULL),
                                                x, y, width, height,
                                                phase_x, phase_y, lop, plane);
                    if (code < 0)
                        return code;
                }
                return 0;
            }
            if ((mdev->num_planes == 4) && (mdev->plane_depth == 1))
            {
                lop = cmykrop[lop & 0xff] | (lop & ~0xff);
                return planar_cmyk4bit_strip_copy_rop(mdev, sdata, sourcex,
                                                      sraster, id, scolors,
                                                      textures, tcolors,
                                                      x, y, width, height,
                                                      phase_x, phase_y, lop);
            }
        }
        /* Fall back to the default implementation (the only one that
         * guarantees to properly cope with planar data). */
        return mem_default_strip_copy_rop(dev, sdata, sourcex, sraster,
                                          id, scolors, textures, tcolors,
                                          x, y, width, height,
                                          phase_x, phase_y, lop);
    }
    /* Extract the plane, and sanitise the lop */
    plane = lop>>lop_planar_shift;
    lop &= ~((plane<<lop_planar_shift) | lop_planar);
    if ((plane < 0) || (plane >= mdev->num_planes))
        return gs_error_rangecheck;
    return plane_strip_copy_rop(mdev, sdata, sourcex, sraster, id, scolors,
                                textures, tcolors, x, y, width, height,
                                phase_x, phase_y, lop, plane);
}

/*
 * Repack planar into chunky format.  This is an internal procedure that
 * implements the straightforward chunky case of get_bits_rectangle, and
 * is also used for the general cases.
 */
static int
planar_to_chunky(gx_device_memory *mdev, int x, int y, int w, int h,
                 int offset, uint draster, byte *dest)
{
    int num_planes = mdev->num_planes;
    sample_load_declare(sptr[GX_DEVICE_COLOR_MAX_COMPONENTS],
                        sbit[GX_DEVICE_COLOR_MAX_COMPONENTS]);
    sample_store_declare(dptr, dbit, dbbyte);
    int ddepth = mdev->color_info.depth;
    int direct =
        (mdev->color_info.depth != num_planes * mdev->plane_depth ? 0 :
         mdev->planes[0].shift == 0 ? -mdev->plane_depth : mdev->plane_depth);
    int pi, ix, iy;

    /* Check whether the planes are of equal size and sequential. */
    /* If direct != 0, we already know they exactly fill the depth. */
    if (direct < 0) {
        for (pi = 0; pi < num_planes; ++pi)
            if (mdev->planes[pi].shift != pi * -direct) {
                direct = 0; break;
            }
    } else if (direct > 0) {
        for (pi = 0; pi < num_planes; ++pi)
            if (mdev->planes[num_planes - 1 - pi].shift != pi * direct) {
                direct = 0; break;
            }
    }
    for (iy = y; iy < y + h; ++iy) {
        byte **line_ptr = mdev->line_ptrs + iy;

        for (pi = 0; pi < num_planes; ++pi, line_ptr += mdev->height) {
            int plane_depth = mdev->planes[pi].depth;
            int xbit = x * plane_depth;

            sptr[pi] = *line_ptr + (xbit >> 3);
            sample_load_setup(sbit[pi], xbit & 7, plane_depth);
        }
        {
            int xbit = offset * ddepth;

            dptr = dest + (iy - y) * draster + (xbit >> 3);
            sample_store_setup(dbit, xbit & 7, ddepth);
        }
        if (direct == -8) {
            /* 1 byte per component, lsb first. */
            switch (num_planes) {
            case 3: {
                const byte *p0 = sptr[2];
                const byte *p1 = sptr[1];
                const byte *p2 = sptr[0];

                for (ix = w; ix > 0; --ix, dptr += 3) {
                    dptr[0] = *p0++;
                    dptr[1] = *p1++;
                    dptr[2] = *p2++;
                }
            }
            continue;
            case 4:
                for (ix = w; ix > 0; --ix, dptr += 4) {
                    dptr[0] = *sptr[3]++;
                    dptr[1] = *sptr[2]++;
                    dptr[2] = *sptr[1]++;
                    dptr[3] = *sptr[0]++;
                }
                continue;
            default:
                break;
            }
        }
        sample_store_preload(dbbyte, dptr, dbit, ddepth);
        for (ix = w; ix > 0; --ix) {
            gx_color_index color = 0;

            for (pi = 0; pi < num_planes; ++pi) {
                int plane_depth = mdev->planes[pi].depth;
                uint value;

                sample_load_next16(value, sptr[pi], sbit[pi], plane_depth);
                color |= (gx_color_index)value << mdev->planes[pi].shift;
            }
            sample_store_next_any(color, dptr, dbit, ddepth, dbbyte);
        }
        sample_store_flush(dptr, dbit, ddepth, dbbyte);
    }
    return 0;
}

/* Copy bits back from a planar memory device. */
static int
mem_planar_get_bits_rectangle(gx_device * dev, const gs_int_rect * prect,
                              gs_get_bits_params_t * params,
                              gs_int_rect ** unread)
{
    /* This duplicates most of mem_get_bits_rectangle.  Tant pis. */
    gx_device_memory * const mdev = (gx_device_memory *)dev;
    gs_get_bits_options_t options = params->options;
    int x = prect->p.x, w = prect->q.x - x, y = prect->p.y, h = prect->q.y - y;
    int num_planes = mdev->num_planes;
    gs_get_bits_params_t copy_params;
    int code;

    if (options == 0) {
        /*
         * Unfortunately, as things stand, we have to support
         * GB_PACKING_CHUNKY.  In fact, we can't even claim to support
         * GB_PACKING_PLANAR, because there is currently no way to
         * describe the particular planar packing format that the device
         * actually stores.
         */
        params->options =
            (GB_ALIGN_STANDARD | GB_ALIGN_ANY) |
            (GB_RETURN_COPY | GB_RETURN_POINTER) |
            (GB_OFFSET_0 | GB_OFFSET_SPECIFIED | GB_OFFSET_ANY) |
            (GB_RASTER_STANDARD | GB_RASTER_SPECIFIED | GB_RASTER_ANY) |
            /*
            (mdev->num_planes == mdev->color_info.depth ?
             GB_PACKING_CHUNKY | GB_PACKING_PLANAR | GB_PACKING_BIT_PLANAR :
             GB_PACKING_CHUNKY | GB_PACKING_PLANAR)
            */
            GB_PACKING_CHUNKY |
            GB_COLORS_NATIVE | GB_ALPHA_NONE;
        return_error(gs_error_rangecheck);
    }
    if ((w <= 0) | (h <= 0)) {
        if ((w | h) < 0)
            return_error(gs_error_rangecheck);
        return 0;
    }
    if (x < 0 || w > dev->width - x ||
        y < 0 || h > dev->height - y
        )
        return_error(gs_error_rangecheck);

    /* First off, see if we can satisfy get_bits_rectangle with just returning
     * pointers to the existing data. */
    {
        gs_get_bits_params_t copy_params;
        byte **base = &scan_line_base(mdev, y);
        int code;

        copy_params.options =
            GB_COLORS_NATIVE | GB_PACKING_PLANAR | GB_ALPHA_NONE |
            (mdev->raster ==
             bitmap_raster(mdev->width * mdev->color_info.depth) ?
             GB_RASTER_STANDARD : GB_RASTER_SPECIFIED);
        copy_params.raster = mdev->raster;
        code = gx_get_bits_return_pointer(dev, x, h, params,
                                          &copy_params, base);
        if (code >= 0)
            return code;
    }

    /*
     * If the request is for exactly one plane, hand it off to a device
     * temporarily tweaked to return just that plane.
     */
    if (!(~options & (GB_PACKING_PLANAR | GB_SELECT_PLANES))) {
        /* Check that only a single plane is being requested. */
        int pi;

        for (pi = 0; pi < num_planes; ++pi)
            if (params->data[pi] != 0)
                break;
        if (pi < num_planes) {
            int plane = pi++;

            for (; pi < num_planes; ++pi)
                if (params->data[pi] != 0)
                    break;
            if (pi == num_planes) {
                mem_save_params_t save;

                copy_params = *params;
                copy_params.options =
                    (options & ~(GB_PACKING_ALL | GB_SELECT_PLANES)) |
                    GB_PACKING_CHUNKY;
                copy_params.data[0] = copy_params.data[plane];
                MEM_SAVE_PARAMS(mdev, save);
                mdev->line_ptrs += mdev->height * plane;
                MEM_SET_PARAMS(mdev, mdev->planes[plane].depth);
                code = mem_get_bits_rectangle(dev, prect, &copy_params,
                                              unread);
                MEM_RESTORE_PARAMS(mdev, save);
                if (code >= 0) {
                    params->data[plane] = copy_params.data[0];
                    return code;
                }
            }
        }
    }
    /*
     * We can't return the requested plane by itself.  Fall back to
     * chunky format.  This is somewhat painful.
     *
     * The code here knows how to produce just one chunky format:
     * GB_COLORS_NATIVE, GB_ALPHA_NONE, GB_RETURN_COPY.
     * For any other format, we generate this one in a buffer and
     * hand it off to gx_get_bits_copy.  This is *really* painful.
     */
    if (!(~options & (GB_COLORS_NATIVE | GB_ALPHA_NONE |
                      GB_PACKING_CHUNKY | GB_RETURN_COPY))) {
        int offset = (options & GB_OFFSET_SPECIFIED ? params->x_offset : 0);
        uint draster =
            (options & GB_RASTER_SPECIFIED ? params->raster :
             bitmap_raster((offset + w) * mdev->color_info.depth));

        planar_to_chunky(mdev, x, y, w, h, offset, draster, params->data[0]);
    } else {
        /*
         * Do the transfer through an intermediate buffer.
         * The buffer must be large enough to hold at least one pixel,
         * i.e., GX_DEVICE_COLOR_MAX_COMPONENTS 16-bit values.
         * The algorithms are very similar to those in copy_color.
         */
#define BUF_LONGS\
  max(100, (GX_DEVICE_COLOR_MAX_COMPONENTS * 2 + sizeof(long) - 1) /\
      sizeof(long))
#define BUF_BYTES (BUF_LONGS * ARCH_SIZEOF_LONG)
        union b_ {
            ulong l[BUF_LONGS];
            byte b[BUF_BYTES];
        } buf;
        int br, bw, bh, cx, cy, cw, ch;
        int ddepth = mdev->color_info.depth;
        uint raster = bitmap_raster(ddepth * mdev->width);
        gs_get_bits_params_t dest_params;
        int dest_bytes;

        if (raster > BUF_BYTES) {
            br = BUF_BYTES;
            bw = BUF_BYTES * 8 / ddepth;
            bh = 1;
        } else {
            br = raster;
            bw = w;
            bh = BUF_BYTES / raster;
        }
        copy_params.options =
            GB_COLORS_NATIVE | GB_PACKING_CHUNKY | GB_ALPHA_NONE |
            GB_RASTER_STANDARD;
        copy_params.raster = raster;
        /* The options passed in from above may have GB_OFFSET_0, and what's
         * more, the code below may insist on GB_OFFSET_0 being set. Hence we
         * can't rely on x_offset to allow for the block size we are using.
         * We'll have to adjust the pointer by steam. */
        dest_params = *params;
        dest_params.x_offset = params->x_offset;
        if (options & GB_COLORS_RGB)
            dest_bytes = 3;
        else if (options & GB_COLORS_CMYK)
            dest_bytes = 4;
        else if (options & GB_COLORS_GRAY)
            dest_bytes = 1;
        else
            dest_bytes = mdev->color_info.depth / mdev->plane_depth;
        /* We assume options & GB_DEPTH_8 */
        for (cy = y; cy < y + h; cy += ch) {
            ch = min(bh, y + h - cy);
            for (cx = x; cx < x + w; cx += cw) {
                cw = min(bw, x + w - cx);
                planar_to_chunky(mdev, cx, cy, cw, ch, 0, br, buf.b);
                code = gx_get_bits_copy(dev, 0, cw, ch, &dest_params,
                                        &copy_params, buf.b, br);
                if (code < 0)
                    return code;
                dest_params.data[0] += cw * dest_bytes;
            }
            dest_params.data[0] += ch * dest_params.raster - (w*dest_bytes);
        }
#undef BUF_BYTES
#undef BUF_LONGS
    }
    return 0;
}
