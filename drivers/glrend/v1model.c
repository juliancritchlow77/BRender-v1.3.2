/*
 * Support routines for rendering models
 */
#include "brassert.h"
#include "drv.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void apply_blend_mode(state_stack* self) {
    /* C_result = (C_source * F_Source) + (C_dest * F_dest) */

    /* NB: srcAlpha and dstAlpha are all GL_ONE and GL_ZERO respectively. */
    switch (self->prim.blend_mode) {
    default:
        /* fallthrough */
    case BRT_BLEND_STANDARD:
        /* fallthrough */
    case BRT_BLEND_DIMMED:
        /*
         * 3dfx blending mode = 1
         * Colour = (alpha * src) + ((1 - alpha) * dest)
         * Alpha  = (1     * src) + (0           * dest)
         */
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        break;

    case BRT_BLEND_SUMMED:
        /*
         * 3fdx blending mode = 4
         * Colour = (alpha * src) + (1 * dest)
         * Alpha  = (1     * src) + (0 * dest)
         */
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        break;

    case BRT_BLEND_PREMULTIPLIED:
        /*
         * 3dfx qblending mode = 2
         * Colour = (1 * src) + ((1 - alpha) * dest)
         * Alpha  = (1 * src) + (0           * dest)
         */
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        break;
    }
    GL_CHECK_ERROR();
}

static void apply_depth_properties(state_stack* state, uint32_t states) {
    br_boolean depth_valid = BR_TRUE; /* Defaulting to BR_TRUE to keep existing behaviour. */
    GLenum depth_test = GL_NONE;

    /* Only use the states we want (if valid). */
    states = state->valid & states;

    if (states & MASK_STATE_OUTPUT) {
        depth_valid = state->output.depth != NULL;
    }

    if (states & MASK_STATE_SURFACE) {
        if (state->surface.force_front || state->surface.force_back)
            depth_test = GL_FALSE;
        else
            depth_test = GL_TRUE;
    }

    if (depth_valid == BR_TRUE) {
        if (depth_test == GL_TRUE)
            glEnable(GL_DEPTH_TEST);
        else if (depth_test == GL_FALSE)
            glDisable(GL_DEPTH_TEST);
    }

    if (states & MASK_STATE_PRIMITIVE) {
        if (state->prim.flags & PRIMF_DEPTH_WRITE)
            glDepthMask(GL_TRUE);
        else
            glDepthMask(GL_FALSE);

        GLenum depthFunc;
        switch (state->prim.depth_test) {
        case BRT_LESS:
            depthFunc = GL_LESS;
            break;
        case BRT_GREATER:
            depthFunc = GL_GREATER;
            break;
        case BRT_LESS_OR_EQUAL:
            depthFunc = GL_LEQUAL;
            break;
        case BRT_GREATER_OR_EQUAL:
            depthFunc = GL_GEQUAL;
            break;
        case BRT_EQUAL:
            depthFunc = GL_EQUAL;
            break;
        case BRT_NOT_EQUAL:
            depthFunc = GL_NOTEQUAL;
            break;
        case BRT_NEVER:
            depthFunc = GL_NEVER;
            break;
        case BRT_ALWAYS:
            depthFunc = GL_ALWAYS;
            break;
        default:
            depthFunc = GL_LESS;
        }
        glDepthFunc(depthFunc);
    }
    GL_CHECK_ERROR();
}

/*
 * Texture dump and texture pack support, for trying upscaled textures. Files
 * are uncompressed 32 bit TGA, named <texture>_<width>x<height>.tga after the
 * texture's name and its original size.
 */
static void texpack_path(char* out, size_t size, const char* dir, const char* ident, int w, int h) {
    size_t n = 0;
    const char* c;

    n = (size_t)snprintf(out, size, "%s/", dir);
    for (c = ident != NULL ? ident : "unnamed"; *c != '\0' && n + 32 < size; c++) {
        out[n++] = (isalnum((unsigned char)*c) || *c == '-') ? *c : '_';
    }
    snprintf(out + n, size - n, "_%dx%d.tga", w, h);
}

static void texpack_dump(HVIDEO hVideo, br_pixelmap* src, const br_uint_32* palette) {
    char path[512];
    FILE* f;
    uint8_t header[18] = { 0 };
    uint8_t* row;
    int x, y;

    texpack_path(path, sizeof(path), hVideo->textureDumpDir, src->identifier, src->width, src->height);
    f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    header[2] = 2;
    header[12] = src->width & 0xff;
    header[13] = src->width >> 8;
    header[14] = src->height & 0xff;
    header[15] = src->height >> 8;
    header[16] = 32;
    header[17] = 0x28; /* top left origin, 8 bits of alpha */
    fwrite(header, 1, sizeof(header), f);
    row = (uint8_t*)malloc((size_t)src->width * 4);
    if (row != NULL) {
        for (y = 0; y < src->height; y++) {
            for (x = 0; x < src->width; x++) {
                int index = ((br_uint_8*)src->pixels)[y * src->row_bytes + x];
                br_uint_32 c = palette[index];
                row[x * 4 + 0] = BR_BLU(c);
                row[x * 4 + 1] = BR_GRN(c);
                row[x * 4 + 2] = BR_RED(c);
                row[x * 4 + 3] = index == 0 ? 0 : 255;
            }
            fwrite(row, 1, (size_t)src->width * 4, f);
        }
        free(row);
    }
    fclose(f);
}

/* Reads a 24 or 32 bit uncompressed TGA as RGBA bytes. Returns NULL if it isn't one. */
static br_uint_32* texpack_load(const char* path, int* out_w, int* out_h) {
    FILE* f = fopen(path, "rb");
    uint8_t header[18];
    int w, h, bpp, x, y;
    uint8_t* raw;
    uint8_t* out;

    if (f == NULL) {
        return NULL;
    }
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || header[2] != 2 || (header[16] != 32 && header[16] != 24)) {
        fclose(f);
        return NULL;
    }
    w = header[12] | (header[13] << 8);
    h = header[14] | (header[15] << 8);
    bpp = header[16] / 8;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        fclose(f);
        return NULL;
    }
    fseek(f, header[0], SEEK_CUR);
    raw = (uint8_t*)malloc((size_t)w * h * bpp);
    out = (uint8_t*)malloc((size_t)w * h * 4);
    if (raw == NULL || out == NULL || fread(raw, bpp, (size_t)w * h, f) != (size_t)w * h) {
        free(raw);
        free(out);
        fclose(f);
        return NULL;
    }
    fclose(f);
    for (y = 0; y < h; y++) {
        int sy = (header[17] & 0x20) ? y : h - 1 - y;
        for (x = 0; x < w; x++) {
            const uint8_t* s = raw + ((size_t)sy * w + x) * bpp;
            uint8_t* d = out + ((size_t)y * w + x) * 4;
            uint8_t a = bpp == 4 ? s[3] : 255;
            /*
             * Colour-keyed textures are drawn by discarding black, so a see-through
             * pixel becomes black and a solid one is kept from being exactly black.
             */
            if (a < 128) {
                d[0] = d[1] = d[2] = 0;
            } else {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                if (d[0] == 0 && d[1] == 0 && d[2] == 0) {
                    d[0] = d[1] = d[2] = 1;
                }
            }
            d[3] = 255;
        }
    }
    free(raw);
    *out_w = w;
    *out_h = h;
    return (br_uint_32*)out;
}

static br_uint_32 texpack_hash(br_pixelmap* src) {
    br_uint_32 h = 2166136261u;
    int x, y;

    for (y = 0; y < src->height; y++) {
        const br_uint_8* row = (const br_uint_8*)src->pixels + y * src->row_bytes;
        for (x = 0; x < src->width; x++) {
            h = (h ^ row[x]) * 16777619u;
        }
    }
    return h;
}

/*
 * Take a pixelmap and palette and convert 8 bit to 32 bit just in time, or, if
 * the texture pack has a replacement for it, use that.
 */
static void update_paletted_texture(HVIDEO hVideo, br_buffer_stored* tex, br_uint_32 *palette) {
    br_pixelmap* src = tex->source;

    if (hVideo->textureDumpDir != NULL) {
        texpack_dump(hVideo, src, palette);
    }
    if (hVideo->texturePackDir != NULL && !tex->replacement_tried) {
        char path[512];
        tex->replacement_tried = BR_TRUE;
        texpack_path(path, sizeof(path), hVideo->texturePackDir, src->identifier, src->width, src->height);
        tex->replacement = texpack_load(path, &tex->replacement_w, &tex->replacement_h);
        tex->replacement_hash = texpack_hash(src);
    } else if (tex->replacement != NULL && texpack_hash(src) != tex->replacement_hash) {
        /*
         * The game has drawn into the texture since: it is animated (the rear
         * view camera, a video), so a fixed picture would freeze it.
         */
        free(tex->replacement);
        tex->replacement = NULL;
        /* the texture is now the replacement's size: go back to the game's */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src->width, src->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    if (tex->replacement != NULL) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex->replacement_w, tex->replacement_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex->replacement);
        glGenerateMipmap(GL_TEXTURE_2D);
        GL_CHECK_ERROR();
        return;
    }

    uint32_t* buffer = BrScratchAllocate(sizeof(uint32_t) * src->width * src->height);
    uint32_t* buffer_ptr = buffer;
    br_uint_8* src_px = src->pixels;

    for (int y = 0; y < src->height; y++) {
        for (int x = 0; x < src->width; x++) {
            int index = src_px[y * src->row_bytes + x];
            *buffer_ptr = (0xff000000 | BR_BLU(palette[index]) << 16 | BR_GRN(palette[index]) << 8 | BR_RED(palette[index]));
            buffer_ptr++;
        }
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src->width, src->height, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
    glGenerateMipmap(GL_TEXTURE_2D);
    BrScratchFree(buffer);
    GL_CHECK_ERROR();
}

static void apply_stored_properties(HVIDEO hVideo, state_stack* state, uint32_t states, shader_data_model* model, GLuint tex_default) {
    br_boolean blending_on;

    /* Only use the states we want (if valid). */
    states = state->valid & states;

    if (states & MASK_STATE_CULL) {
        /*
         * Apply culling states. These are a bit confusing:
         * BRT_ONE_SIDED - Simple, cull back faces. From BRT_ONE_SIDED.
         *
         * BRT_TWO_SIDED - This means the face is two-sided, not to cull
         *                 both sides. From BR_MATF_TWO_SIDED. In the .3ds file
         *                 format, the "two sided" flag means the material is
         *                 visible from the back, or "not culled". fmt/load3ds.c
         *                 sets BR_MATF_TWO_SIDED if this is set, so assume this is
         *                 the correct behaviour.
         *
         * BRT_NONE      - Confusing, this is set if the material has
         *                 BR_MATF_ALWAYS_VISIBLE, but is overridden if
         *                 BR_MATF_TWO_SIDED is set. Assume it means the same
         *                 as BR_MATF_TWO_SIDED.
         */
        switch (state->cull.type) {
        case BRT_ONE_SIDED:
        default: /* Default BRender policy, so default. */
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            break;

        case BRT_TWO_SIDED:
        case BRT_NONE:
            glDisable(GL_CULL_FACE);
            break;
        }
    }

    if (states & MASK_STATE_SURFACE) {
        glActiveTexture(GL_TEXTURE0);

        if (state->surface.colour_source == BRT_SURFACE) {
            br_uint_32 colour = state->surface.colour;
            float r = BR_RED(colour) / 255.0f;
            float g = BR_GRN(colour) / 255.0f;
            float b = BR_BLU(colour) / 255.0f;
            //
            BrVector4Set(&model->surface_colour, r, g, b, state->surface.opacity);
        } else {
            BrVector4Set(&model->surface_colour, 0.0f, 1.0f, 1.0f, state->surface.opacity);
        }

        model->ka = state->surface.ka;
        model->ks = state->surface.ks;
        model->kd = state->surface.kd;
        model->power = state->surface.power;

        switch (state->surface.mapping_source) {
        case BRT_GEOMETRY_MAP:
        default:
            model->uv_source = 0;
            break;

        case BRT_ENVIRONMENT_LOCAL:
            model->uv_source = 1;
            break;

        case BRT_ENVIRONMENT_INFINITE:
            model->uv_source = 2;
            break;
        }

        BrMatrix4Copy23(&model->map_transform, &state->surface.map_transform);

        model->prelit = state->surface.prelighting;
        model->lighting = state->surface.lighting;
    }

    if (states & MASK_STATE_PRIMITIVE) {

        if (state->prim.flags & PRIMF_COLOUR_WRITE)
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        else
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        if (state->prim.colour_map) {
            model->disable_colour_key = !(state->prim.flags & PRIMF_COLOUR_KEY);

            glBindTexture(GL_TEXTURE_2D, BufferStoredGLGetTexture(state->prim.colour_map));

            // has the 8 bit color source changed?
            if (state->prim.colour_map->paletted_source_dirty == BR_TRUE) {
                update_paletted_texture(hVideo, state->prim.colour_map, state->prim.colour_map->palette_pointer->entries);
                state->prim.colour_map->paletted_source_dirty = BR_FALSE;
                state->prim.colour_map->palette_revision = state->prim.colour_map->palette_pointer->revision;
            }
            // or has the palette changed?
            else if (state->prim.colour_map->palette_pointer != NULL && state->prim.colour_map->palette_revision != state->prim.colour_map->palette_pointer->revision) {
                update_paletted_texture(hVideo, state->prim.colour_map, state->prim.colour_map->palette_pointer->entries);
                state->prim.colour_map->palette_revision = state->prim.colour_map->palette_pointer->revision;
            }

            // glUniform1i(hVideo->brenderProgram.uniforms.main_texture, hVideo->brenderProgram.mainTextureBinding);
            model->disable_texture = 0;

        } else {

            // todo?
            model->disable_colour_key = 0;
            glBindTexture(GL_TEXTURE_2D, tex_default);
            model->disable_texture = 1;
            // BrVector4Set(&model->surface_colour, 0, 1, 0, 1);
            // glBindTexture(GL_TEXTURE_2D, 27);
            // glUniform1i(hVideo->brenderProgram.uniforms.main_texture, hVideo->brenderProgram.mainTextureBinding);
        }

        GLenum minFilter, magFilter;
        GLfloat maxAnisotropy;
        if (state->prim.filter == BRT_LINEAR && state->prim.mip_filter == BRT_LINEAR) {
            minFilter = GL_LINEAR_MIPMAP_LINEAR;
            magFilter = GL_LINEAR;
            maxAnisotropy = hVideo->maxAnisotropy;
        } else if (state->prim.filter == BRT_LINEAR && state->prim.mip_filter == BRT_NONE) {
            minFilter = GL_LINEAR;
            magFilter = GL_LINEAR;
            maxAnisotropy = 1.0f;
        } else if (state->prim.filter == BRT_NONE && state->prim.mip_filter == BRT_LINEAR) {
            minFilter = GL_NEAREST_MIPMAP_NEAREST;
            magFilter = GL_NEAREST;
            maxAnisotropy = hVideo->maxAnisotropy;
        } else if (state->prim.filter == BRT_NONE && state->prim.mip_filter == BRT_NONE) {
            minFilter = GL_NEAREST;
            magFilter = GL_NEAREST;
            maxAnisotropy = 1.0f;
        } else {
            assert(0);
        }

        // Application override. Colour-keyed textures (fences, trees, wire) keep
        // their hard edges, since smoothing would blend the key colour into them.
        if (hVideo->textureFilter != 0 && state->prim.colour_map != NULL && !(state->prim.flags & PRIMF_COLOUR_KEY)) {
            minFilter = GL_LINEAR_MIPMAP_LINEAR;
            magFilter = GL_LINEAR;
            maxAnisotropy = hVideo->textureFilter >= 2 ? hVideo->maxAnisotropy : 1.0f;
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magFilter);

        if (GLAD_GL_EXT_texture_filter_anisotropic)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);

        blending_on = (state->prim.flags & PRIMF_BLEND) || (state->prim.colour_map != NULL && state->prim.colour_map->blended);
        if (blending_on) {
            glEnable(GL_BLEND);
            model->alpha = state->prim.alpha_val / 255.0f;
            apply_blend_mode(state);
        } else {
            glDisable(GL_BLEND);
        }

        model->fog_enabled = state->prim.fog_enabled;
        BrVector3Set(&model->fog_colour, BR_RED(state->prim.fog_colour) / 255.0f, BR_GRN(state->prim.fog_colour) / 255.0f, BR_BLU(state->prim.fog_colour) / 255.0f);
        model->fog_min = state->prim.fog_min;
        model->fog_max = state->prim.fog_max;
    }
    apply_depth_properties(state, states);
    GL_CHECK_ERROR();
}

void StoredGLRenderGroup(br_geometry_stored* self, br_renderer* renderer, const gl_groupinfo* groupinfo) {
    state_cache* cache = &renderer->state.cache;
    br_device_pixelmap* screen = renderer->pixelmap->screen;
    HVIDEO hVideo = &screen->asFront.video;
    br_renderer_state_stored* stored = groupinfo->stored;
    br_boolean unlit;
    shader_data_model model;

    /* Update the per-model cache (matrices and lights) */
    StateGLUpdateModel(cache, &renderer->state.current->matrix);

#if DEBUG
    { /* Check that sceneBegin() actually did it's shit. */

        /* Program */
        GLint p;
        glGetIntegerv(GL_CURRENT_PROGRAM, &p);
        ASSERT(p == hVideo->brenderProgram.program);

        /* FBO */
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &p);
        ASSERT(p == renderer->state.current->output.colour->asBack.glFbo);

        /* Model UBO */
        glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &p);
        ASSERT(p == hVideo->brenderProgram.uboModel);
    }
#endif

    model.projection_brender = cache->model.p_br;
    model.projection = cache->model.p;
    model.model_view = cache->model.mv;
    model.mvp = cache->model.mvp;
    model.normal_matrix = cache->model.normal;
    model.environment_matrix = cache->model.environment;
    model.eye_m = cache->model.eye_m;

    glBindVertexArray(self->gl_vao);

    if (stored) {
        // jeff: disable culling to match behavior of existing hardware drivers
        apply_stored_properties(hVideo, &stored->state, MASK_STATE_PRIMITIVE | MASK_STATE_SURFACE /*| MASK_STATE_CULL */,
            &model, screen->asFront.tex_white);
    } else {
        /* If there's no stored state, apply all states from global. */
        GLuint default_tex;
        if (groupinfo->default_state->state.prim.colour_map) {
            default_tex = groupinfo->default_state->state.prim.colour_map->gl_tex;
        } else {
            default_tex = renderer->pixelmap->asFront.tex_white;
        }

        renderer->state.current->surface = groupinfo->default_state->state.surface;
        renderer->state.current->prim = groupinfo->default_state->state.prim;
        renderer->state.current->cull = groupinfo->default_state->state.cull;
        // Jeff: disable culling to match behavior of existing hardware drivers
        apply_stored_properties(hVideo, renderer->state.current, MASK_STATE_PRIMITIVE | MASK_STATE_SURFACE /*| MASK_STATE_CULL */, &model, default_tex);
    }

    BrVector4Set(&model.clear_colour, renderer->pixelmap->asBack.clearColour[0], renderer->pixelmap->asBack.clearColour[1],
        renderer->pixelmap->asBack.clearColour[2], renderer->pixelmap->asBack.clearColour[3]);

    glBufferData(GL_UNIFORM_BUFFER, sizeof(model), &model, GL_STATIC_DRAW);
    glDrawElements(GL_TRIANGLES, groupinfo->count, GL_UNSIGNED_SHORT, groupinfo->offset);

    renderer->scene_stats.face_group_count++;
    renderer->scene_stats.triangles_rendered_count += groupinfo->group->nfaces;
    renderer->scene_stats.triangles_drawn_count += groupinfo->group->nfaces;
    renderer->scene_stats.vertices_rendered_count += groupinfo->group->nfaces * 3;
    GL_CHECK_ERROR();
}
