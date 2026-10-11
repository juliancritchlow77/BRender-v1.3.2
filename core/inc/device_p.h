#ifndef _DEVICE_P_H_
#define _DEVICE_P_H_

/*
    This file was added by dethrace.
    Nasty hack to provide device-specific types to external consumers of BRender
*/

#include "compiler.h"
#include "pixelmap.h"

// For use with `virtualframebuffer` device.
typedef void br_device_virtualfb_swapbuffers_cbfn(br_pixelmap* pm);
typedef void br_device_virtualfb_palette_changed_cbfn(br_uint_32* palette_entries);

typedef struct br_device_virtualfb_callback_procs {
    br_device_virtualfb_palette_changed_cbfn      *palette_changed;
    br_device_virtualfb_swapbuffers_cbfn          *swap_buffers;
} br_device_virtualfb_callback_procs;

// For use with `glrend` device.
typedef void BR_CALLBACK br_device_gl_swapbuffers_cbfn(br_pixelmap* pm);
typedef void* BR_CALLBACK br_device_gl_getprocaddress_cbfn(const char* name);
typedef void BR_CALLBACK br_device_gl_free_cbfn(br_pixelmap* pm, void* user);
typedef void BR_CALLBACK br_device_gl_getviewport_cbfn(int *x, int *y, float *width_multiplier, float *height_multiplier);

typedef struct br_device_gl_callback_procs {
    br_device_gl_getprocaddress_cbfn      *get_proc_address;
    br_device_gl_getviewport_cbfn         *get_viewport;
    br_device_gl_swapbuffers_cbfn         *swap_buffers;
    br_device_gl_free_cbfn                *free;

    /*
     * Presentation settings, chosen by the application. Zero, which is what an
     * application that does not set them gets, is the original behaviour.
     *
     * scale_filter, for the 2D layer (menus, HUD, text, videos) stretched over
     * the window: 0 nearest, 1 sharp bilinear (crisp pixels with smooth edges
     * between them), 2 bilinear.
     *
     * texture_filter, for 3D textures that are not colour-keyed: 0 as each
     * material asks, 1 smooth (bilinear with mipmaps), 2 smooth and anisotropic.
     */
    int                                    scale_filter;
    int                                    texture_filter;

    /*
     * Texture dumping and replacement, for trying upscaled textures. Both are
     * folders of 32 bit TGA files named <texture>_<width>x<height>.tga, or NULL.
     * texture_dump_dir: each 8 bit texture is written there as it is first used
     * (alpha 0 where the colour key index is). texture_pack_dir: a file there
     * with a texture's name and original size replaces it, at any larger size.
     */
    const char*                            texture_dump_dir;
    const char*                            texture_pack_dir;
} br_device_gl_callback_procs;

#endif
