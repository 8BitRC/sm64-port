#ifdef TARGET_XBOX

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <windows.h>
#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h>
#include <pbkit/pbkit.h>
#include <xgu/xgu.h>
#include <xgu/xgux.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_rendering_api.h"

#include "gfx_xbox.h"
#include "gfx_cc.h"
#include "macros.h"

#include "swizzle.h"

#define NV_TEX_FILTER_ANISO  4
#define NV_TEX_FILTER_LINEAR 2
#define NV_TEX_FILTER_NONE   1
#define NV_TEX_ALPHAKILL     4

#define MAX_SHADERS 64
#define MAX_TEXTURES 512
#define MAX_ATTRIBS 8
#define MAX_VERTS (2048 * 3)

#define TEXCACHE_SIZE (512 * 64 * 32 * 4)
#define VTXBUF_FLOATS (MAX_VERTS * 32)

extern int win_width;
extern int win_height;

typedef uint32_t *(*combiner_fn_t)(uint32_t *);

struct CompiledShader {
    const uint32_t shader_id;
    const XguTransformProgramInstruction *vp_inst;
    const uint32_t *vp_size;
    combiner_fn_t fp_combiner;
};

// this contains `struct CompiledShader shader_objs[]` and `u32 num_shader_objs`
#include "xbox_shader_db.h"

struct VertexAttrib {
    uint32_t ofs;
    uint32_t type;
    uint32_t size;
};

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc;

    const struct CompiledShader *prog;

    struct VertexAttrib attr[MAX_ATTRIBS];
    uint32_t num_attrs;
    uint32_t stride;
};

struct Texture {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t wshift;
    uint32_t hshift;
    uint32_t size;
    uint32_t wrap_u;
    uint32_t wrap_v;
    uint32_t filter;
    uint32_t format;
    uint32_t addr;
    uint8_t *data;
};

struct Rect {
    float x, y;
    float w, h;
};

static struct RenderState {
    struct Rect view;
    bool view_changed;
    struct Rect clip;
    bool clip_changed;

    struct Texture *tex[2];
    struct Texture *last_tex;
    uint32_t last_tile;
    bool tex_changed[2];

    struct ShaderProgram *shader;
    bool shader_changed;

    bool blend, blend_changed;
    bool ztest, ztest_changed;
    bool zmask, zmask_changed;
    bool decal, decal_changed;
    bool atest, atest_changed;
    bool flags_changed;

    XguVec4 u_view_scale;
    XguVec4 u_view_ofs;
    bool uniforms_changed;
} rst;

static struct ShaderProgram shader_program_pool[MAX_SHADERS];
static uint32_t num_shaders;

static struct Texture tex_pool[MAX_TEXTURES];
static uint32_t num_textures;

static uint8_t *tex_cache = NULL;
static uint8_t *tex_cache_ptr = NULL;
static uint8_t *tex_cache_end = NULL;

static float *vtx_buf;
static float *vtx_buf_ptr;
static float *vtx_buf_end;
static int vtx_buf_cur;

/* from stackoverflow.com/a/11398748 */

static int log2_u32(uint32_t value) {
    static const int tab32[32] = {
         0,  9,  1, 10, 13, 21,  2, 29,
        11, 14, 16, 18, 22, 25,  3, 30,
         8, 12, 20, 28, 15, 17, 24,  7,
        19, 27, 23,  6, 26,  5,  4, 31
    };
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    return tab32[(uint32_t)(value*0x07C4ACDD) >> 27];
}

static inline void draw_finish(void) {
    while (pb_busy());
}

static inline void draw_push_wait(void) {
    uint32_t *cmd = pb_begin();
    cmd = xgu_wait_for_idle(cmd);
    pb_end(cmd);
}

uint32_t* draw_set_texture_control0(uint32_t* p, unsigned int texture_index, bool enable, bool alphakill, uint16_t min_lod, uint16_t max_lod) {
    assert(texture_index < XGU_TEXTURE_COUNT);
    return push_command_parameter(p, NV097_SET_TEXTURE_CONTROL0 + texture_index*64,
                                  (enable ? NV097_SET_TEXTURE_CONTROL0_ENABLE : 0) |
                                  XGU_MASK(NV097_SET_TEXTURE_CONTROL0_MIN_LOD_CLAMP, min_lod) |
                                  XGU_MASK(NV097_SET_TEXTURE_CONTROL0_MAX_LOD_CLAMP, max_lod) |
                                  (alphakill ? NV_TEX_ALPHAKILL : 0));
}

static inline uint32_t *draw_set_texture(uint32_t *cmd, const uint32_t i, const struct Texture *tex) {
    if (tex && tex->data) {
        cmd = draw_set_texture_control0(cmd, i, true, rst.atest, 0, 0);
        cmd = xgu_set_texture_offset(cmd, i, (const void *)tex->addr);
        cmd = xgu_set_texture_format(cmd, i, 2, false, XGU_SOURCE_COLOR, 2, tex->format, 1, tex->wshift, tex->hshift, 0);
        cmd = xgu_set_texture_address(cmd, i, tex->wrap_u, false, tex->wrap_v, false, XGU_CLAMP_TO_EDGE, false, false);
        cmd = xgu_set_texture_filter(cmd, i, 0, XGU_TEXTURE_CONVOLUTION_QUINCUNX, tex->filter, tex->filter, false, false, false, false);
    } else {
        // FIXME: disabling the texture makes this die on real hardware
        //        maybe wait until the current drawcall finishes before doing that?
        // cmd = draw_set_texture_control0(cmd, i, false, false, 0, 0);
    }
    return cmd;
}

static inline uint32_t *draw_set_blending(uint32_t *cmd, const bool do_blend) {
    cmd = xgu_set_blend_enable(cmd, do_blend);
    cmd = xgu_set_blend_func_sfactor(cmd, XGU_FACTOR_SRC_ALPHA);
    cmd = xgu_set_blend_func_dfactor(cmd, XGU_FACTOR_ONE_MINUS_SRC_ALPHA);
    return cmd;
}

static inline uint32_t *draw_set_ztest(uint32_t *cmd, const bool z_test) {
    return xgu_set_depth_test_enable(cmd, z_test);
}

static inline uint32_t *draw_set_zmask(uint32_t *cmd, const bool z_mask) {
    return xgu_set_depth_mask(cmd, z_mask);
}

static inline uint32_t *draw_set_atest(uint32_t *cmd, const bool a_test) {
    return xgu_set_alpha_test_enable(cmd, a_test);
}

static inline uint32_t *draw_set_polygon_offset(uint32_t *cmd, const bool enabled, const float scale, const float ofs) {
    cmd = push_command_boolean(cmd, NV097_SET_POLY_OFFSET_FILL_ENABLE, enabled);
    if (enabled) {
        cmd = push_command_float(cmd, NV097_SET_POLYGON_OFFSET_SCALE_FACTOR, scale);
        cmd = push_command_float(cmd, NV097_SET_POLYGON_OFFSET_BIAS, ofs);
    }
    return cmd;
}

static inline void draw_set_vertex_shader(const XguTransformProgramInstruction *inst, const uint32_t size) {
    uint32_t *cmd = pb_begin();
    cmd = xgu_set_transform_program_start(cmd, 0);
    cmd = xgu_set_transform_program_cxt_write_enable(cmd, false);
    pb_end(cmd);

    cmd = pb_begin();
    cmd = xgu_set_transform_program_load(cmd, 0);

    // FIXME: wait for xgu_set_transform_program to get fixed
    for (uint32_t i = 0; i < size / 16; i++) {
        cmd = push_command(cmd, NV097_SET_TRANSFORM_PROGRAM, 4);
        cmd = push_parameters(cmd, &inst[i].i[0], 4);
    }

    pb_end(cmd);
}

static inline void draw_set_combiner(combiner_fn_t combiner) {
    uint32_t *cmd = pb_begin();
    cmd = combiner(cmd);
    pb_end(cmd);
}

static inline void draw_set_uniforms(const struct ShaderProgram *prg) {
    uint32_t *cmd = pb_begin();
    cmd = xgu_set_transform_constant_load(cmd, 96);
    cmd = xgu_set_transform_constant(cmd, (XguVec4*)&rst.u_view_scale, 1);
    cmd = xgu_set_transform_constant(cmd, (XguVec4*)&rst.u_view_ofs, 1);
    pb_end(cmd);
}

static inline void draw_set_vertex_attribs(const struct ShaderProgram *prg) {
    for (uint32_t i = 0; i < prg->num_attrs; ++i) {
        if (prg->attr[i].size)
            xgux_set_attrib_pointer(prg->attr[i].type, XGU_FLOAT, prg->attr[i].size, prg->stride, vtx_buf_ptr + prg->attr[i].ofs);
    }
}

static inline void draw_reset_vertex_attribs(void) {
    for(int i = 0; i < XGU_ATTRIBUTE_COUNT; i++)
        xgux_set_attrib_pointer(i, XGU_FLOAT, 0, 0, NULL);
}

static inline void draw_set_shader(struct ShaderProgram *prg) {
    if (prg && prg->prog) {
        draw_set_vertex_shader(prg->prog->vp_inst, *prg->prog->vp_size);
        draw_set_combiner(prg->prog->fp_combiner);
    }
}

static void draw_update_state(void) {
    uint32_t *cmd;

    if (rst.shader_changed) {
        draw_set_shader(rst.shader);
        rst.shader_changed = false;
    }

    if (rst.flags_changed) {
        cmd = pb_begin();
        if (rst.blend_changed) {
            cmd = draw_set_blending(cmd, rst.blend);
            rst.blend_changed = false;
        }
        if (rst.atest_changed) {
            cmd = draw_set_atest(cmd, rst.atest);
            rst.atest_changed = false;
        }
        if (rst.ztest_changed) {
            cmd = draw_set_ztest(cmd, rst.ztest);
            rst.ztest_changed = false;
        }
        if (rst.zmask_changed) {
            cmd = draw_set_zmask(cmd, rst.zmask);
            rst.zmask_changed = false;
        }
        if (rst.decal_changed) {
            cmd = draw_set_polygon_offset(cmd, rst.decal, -2.f, -2.f);
            rst.decal_changed = false;
        }
        pb_end(cmd);
        rst.flags_changed = false;
    }

    if (rst.tex_changed[0] || rst.tex_changed[1]) {
        cmd = pb_begin();
        for (uint32_t i = 0; i < 2; ++i) {
            if (rst.tex_changed[i]) {
                cmd = draw_set_texture(cmd, i, rst.shader->cc.used_textures[i] ? rst.tex[i] : NULL);
                rst.tex_changed[i] = false;
            }
        }
        pb_end(cmd);
    }

    if (rst.view_changed) {
        // ?
        rst.view_changed = false;
    }

    if (rst.clip_changed) {
        // TODO
        rst.clip_changed = false;
    }

    if (rst.uniforms_changed) {
        draw_set_uniforms(rst.shader);
        rst.uniforms_changed = false;
    }
}

static bool gfx_xbox_rapi_z_is_from_0_to_1(void) {
    return true;
}

static void gfx_xbox_rapi_unload_shader(struct ShaderProgram *old_prg) {
    if (rst.shader && (rst.shader == old_prg || !old_prg))
        rst.shader = NULL;
}

static void gfx_xbox_rapi_load_shader(struct ShaderProgram *new_prg) {
    rst.shader = new_prg;
    rst.atest = new_prg ? new_prg->cc.opt_texture_edge : false;
    rst.flags_changed = rst.atest_changed = true;
    rst.shader_changed = true;
    rst.uniforms_changed = true;
    rst.tex_changed[0] = rst.tex_changed[1] = true;
}

static struct ShaderProgram *gfx_xbox_rapi_create_and_load_new_shader(uint32_t shader_id) {
    const struct CompiledShader *csh = NULL;
    for (uint32_t i = 0; i < num_shader_objs; ++i) {
        if (shader_objs[i].shader_id == shader_id) {
            csh = shader_objs + i;
            break;
        }
    }

    if (!csh) {
        debugPrint("gfx_xbox_rapi_create_and_load_new_shader: could not find shader for id %08x\n", shader_id);
        pb_show_debug_screen();
        while (1) Sleep(100);
    }

    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[num_shaders++];

    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->prog = csh;

    uint32_t num_attrs = 0;
    uint32_t cnt = 0;

    // position always exists
    prg->attr[num_attrs].ofs  = cnt;
    prg->attr[num_attrs].size = 4;
    prg->attr[num_attrs].type = XGU_VERTEX_ARRAY;
    cnt += 4; ++num_attrs;

    if (ccf.used_textures[0] || ccf.used_textures[1]) {
        // texcoords
        prg->attr[num_attrs].ofs  = cnt;
        prg->attr[num_attrs].size = 2;
        prg->attr[num_attrs].type = XGU_TEXCOORD0_ARRAY;
        cnt += 2; ++num_attrs;
    }

    if (ccf.opt_fog) {
        // fog rgb and intensity
        prg->attr[num_attrs].ofs  = cnt;
        prg->attr[num_attrs].size = 4;
        prg->attr[num_attrs].type = XGU_SECONDARY_COLOR_ARRAY;
        cnt += 4; ++num_attrs;
    }

    // all color inputs
    const int csiz = ccf.opt_alpha ? 4 : 3;
    for (int i = 0; i < ccf.num_inputs; i++) {
        prg->attr[num_attrs].ofs  = cnt;
        prg->attr[num_attrs].size = csiz;
        prg->attr[num_attrs].type = XGU_COLOR_ARRAY + i;
        cnt += csiz; ++num_attrs;
    }

    prg->num_attrs = num_attrs;
    prg->stride = cnt * sizeof(float);

    gfx_xbox_rapi_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_xbox_rapi_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < num_shaders; i++)
        if (shader_program_pool[i].shader_id == shader_id)
            return &shader_program_pool[i];
    return NULL;
}

static void gfx_xbox_rapi_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->cc.num_inputs;
    used_textures[0] = prg->cc.used_textures[0];
    used_textures[1] = prg->cc.used_textures[1];
}

static uint32_t gfx_xbox_rapi_new_texture(void) {
    const uint32_t idx = num_textures++;

    if (idx >= MAX_TEXTURES) {
        debugPrint("gfx_xbox_rapi_init: unable to alloc %u bytes for vertex buffer\n", VTXBUF_FLOATS * 4 * 2);
        pb_show_debug_screen();
        while (1) Sleep(100);
    }

    rst.last_tex = tex_pool + idx;
    rst.last_tex->data = NULL;
    rst.last_tex->addr = 0;
    rst.last_tex->wrap_u = XGU_WRAP;
    rst.last_tex->wrap_v = XGU_WRAP;
    rst.last_tex->filter = NV_TEX_FILTER_LINEAR;

    return idx;
}

static void gfx_xbox_rapi_select_texture(int tile, uint32_t texture_id) {
    rst.last_tile = tile;
    rst.last_tex = rst.tex[tile] = tex_pool + texture_id;
    rst.tex_changed[tile] = true;
}

static void gfx_xbox_rapi_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    rst.last_tex->width = width;
    rst.last_tex->height = height;
    rst.last_tex->pitch = width * 4;
    rst.last_tex->wshift = log2_u32(rst.last_tex->width);
    rst.last_tex->hshift = log2_u32(rst.last_tex->height);
    rst.last_tex->format = XGU_TEXTURE_FORMAT_A8B8G8R8_SWIZZLED;

    const uint32_t in_size = height * rst.last_tex->pitch;

    if (tex_cache_ptr + in_size > tex_cache_end) {
        debugPrint("gfx_xbox_rapi_upload_texture(%p, %d, %d): out of cache space!\n", rgba32_buf, width, height);
        tex_cache_ptr = tex_cache; // whatever, just continue from start
    }

    rst.last_tex->data = tex_cache_ptr;
    rst.last_tex->addr = (uint32_t)tex_cache_ptr & 0x03ffffff;

    if ((((uint32_t)width - 1) & (uint32_t)width) || (((uint32_t)height - 1) & (uint32_t)height))
        memset(tex_cache_ptr, 0xff, in_size); // TODO: NPOT scaling
    else
        swizzle_rect(rgba32_buf, width, height, tex_cache_ptr, rst.last_tex->pitch, 4);

    tex_cache_ptr += in_size;
}

static inline uint32_t cm_to_nv(const uint32_t val) {
    if (val & G_TX_MIRROR) return XGU_MIRROR;
    return (val & G_TX_CLAMP) ? XGU_CLAMP_TO_EDGE : XGU_WRAP;
}

static void gfx_xbox_rapi_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    rst.tex[tile]->wrap_u = cm_to_nv(cms);
    rst.tex[tile]->wrap_v = cm_to_nv(cmt);
    rst.tex[tile]->filter = linear_filter ? NV_TEX_FILTER_LINEAR : NV_TEX_FILTER_NONE;
    rst.tex_changed[tile] = true;
}

static void gfx_xbox_rapi_set_depth_test(bool depth_test) {
    rst.ztest = depth_test;
    rst.flags_changed = rst.ztest_changed = true;
}

static void gfx_xbox_rapi_set_depth_mask(bool z_upd) {
    rst.zmask = z_upd;
    rst.flags_changed = rst.zmask_changed = true;
}

static void gfx_xbox_rapi_set_zmode_decal(bool zmode_decal) {
    rst.decal = zmode_decal;
    rst.flags_changed = rst.decal_changed = true;
}

static void gfx_xbox_rapi_set_viewport(int x, int y, int width, int height) {
    rst.view.x = x;
    rst.view.y = y;
    rst.view.w = width;
    rst.view.h = height;
    rst.view_changed = true;
    rst.u_view_scale.x = rst.view.w * 0.5f;
    rst.u_view_scale.y = rst.view.h * -0.5f;
    rst.u_view_scale.z = 16777215.f;
    rst.u_view_scale.w = 1.f;
    rst.u_view_ofs.x = rst.view.x + rst.u_view_scale.x;
    rst.u_view_ofs.y = rst.view.y - rst.u_view_scale.y;
    rst.u_view_ofs.z = 0.f;
    rst.u_view_ofs.w = 0.f;
    rst.uniforms_changed = true;
}

static void gfx_xbox_rapi_set_scissor(int x, int y, int width, int height) {
    rst.clip.x = x;
    rst.clip.y = y;
    rst.clip.w = width;
    rst.clip.h = height;
    rst.clip_changed = true;
}

static void gfx_xbox_rapi_set_use_alpha(bool use_alpha) {
    rst.blend = use_alpha;
    rst.flags_changed = rst.blend_changed = true;
}

static void gfx_xbox_rapi_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (vtx_buf_ptr + buf_vbo_len > vtx_buf_end) {
        draw_finish();
        vtx_buf_ptr = vtx_buf + vtx_buf_cur * VTXBUF_FLOATS;
    }

    draw_update_state();

    memcpy(vtx_buf_ptr, buf_vbo, buf_vbo_len * sizeof(float));

    draw_reset_vertex_attribs();
    draw_set_vertex_attribs(rst.shader);

    xgux_draw_arrays(XGU_TRIANGLES, 0, buf_vbo_num_tris * 3);

    vtx_buf_ptr += buf_vbo_len;
}

static void gfx_xbox_rapi_init(void) {
    vtx_buf = (float *)MmAllocateContiguousMemoryEx(VTXBUF_FLOATS * 4 * 2, 0, 0x03FFAFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    if (!vtx_buf) {
        debugPrint("gfx_xbox_rapi_init: unable to alloc %u bytes for vertex buffer\n", VTXBUF_FLOATS * 4 * 2);
        pb_show_debug_screen();
        while (1) Sleep(100);
    }

    vtx_buf_ptr = vtx_buf;
    vtx_buf_end = vtx_buf + VTXBUF_FLOATS;

    tex_cache = (uint8_t *)MmAllocateContiguousMemoryEx(TEXCACHE_SIZE, 0, 0x03FFAFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    if (!tex_cache) {
        debugPrint("gfx_xbox_rapi_init: unable to alloc %u bytes for texture cache\n", TEXCACHE_SIZE);
        pb_show_debug_screen();
        while (1) Sleep(100);
    }

    tex_cache_ptr = tex_cache;
    tex_cache_end = tex_cache + TEXCACHE_SIZE;

    uint32_t *cmd = pb_begin();
    cmd = xgu_set_transform_execution_mode(cmd, XGU_PROGRAM, XGU_RANGE_MODE_PRIVATE);
    cmd = xgu_set_skin_mode(cmd, XGU_SKIN_MODE_OFF);
    cmd = xgu_set_specular_enable(cmd, true);
    cmd = xgu_set_lighting_enable(cmd, false);
    cmd = xgu_set_cull_face_enable(cmd, false);
    cmd = xgu_set_color_clear_value(cmd, 0xff0000ff);
    cmd = xgu_set_zstencil_clear_value(cmd, 0xffffffff);
    cmd = xgu_set_clip_min(cmd, 0.f);
    cmd = xgu_set_clip_max(cmd, 16777215.f);
    cmd = xgu_set_viewport_offset(cmd, 0.f, 0.f, 0.f, 0.f);
    cmd = xgu_set_alpha_func(cmd, XGU_FUNC_GREATER_OR_EQUAL);
    cmd = xgu_set_alpha_ref(cmd, 255.f * 0.666f);
    cmd = xgu_set_depth_func(cmd, XGU_FUNC_LESS_OR_EQUAL);
    cmd = xgu_set_depth_test_enable(cmd, false);
    cmd = xgu_set_stencil_test_enable(cmd, false);
    pb_end(cmd);

    // disable all textures because apparently sometimes they're still enabled
    cmd = pb_begin();
    for (uint32_t i = 0; i < 4; ++i) {
        cmd = xgu_set_texture_control0(cmd, i, false, 0, 0);
        cmd = xgu_set_texture_matrix_enable(cmd, i, false);
    }
    pb_end(cmd);

    // set all matrices to the identity matrix even though we're not using them
    cmd = pb_begin();
    cmd = xgu_set_projection_matrix(cmd, &mat_identity[0][0]);
    cmd = xgu_set_model_view_matrix(cmd, 0, &mat_identity[0][0]);
    cmd = xgu_set_inverse_model_view_matrix(cmd, 0, &mat_identity[0][0]);
    cmd = xgu_set_composite_matrix(cmd, &mat_identity[0][0]);
    pb_end(cmd);

    draw_finish();
}

static void gfx_xbox_rapi_on_resize(void) {
}

static void gfx_xbox_rapi_start_frame(void) {
    uint32_t *cmd = pb_begin();
    cmd = xgu_clear_surface(cmd, XGU_CLEAR_Z | XGU_CLEAR_STENCIL | XGU_CLEAR_COLOR);
    pb_end(cmd);

    vtx_buf_ptr = vtx_buf + vtx_buf_cur * VTXBUF_FLOATS;
    vtx_buf_end = vtx_buf_ptr + VTXBUF_FLOATS;
}

static void gfx_xbox_rapi_end_frame(void) {
    vtx_buf_cur ^= 1;
}

static void gfx_xbox_rapi_finish_render(void) {
}

struct GfxRenderingAPI gfx_xbox_rapi = {
    gfx_xbox_rapi_z_is_from_0_to_1,
    gfx_xbox_rapi_unload_shader,
    gfx_xbox_rapi_load_shader,
    gfx_xbox_rapi_create_and_load_new_shader,
    gfx_xbox_rapi_lookup_shader,
    gfx_xbox_rapi_shader_get_info,
    gfx_xbox_rapi_new_texture,
    gfx_xbox_rapi_select_texture,
    gfx_xbox_rapi_upload_texture,
    gfx_xbox_rapi_set_sampler_parameters,
    gfx_xbox_rapi_set_depth_test,
    gfx_xbox_rapi_set_depth_mask,
    gfx_xbox_rapi_set_zmode_decal,
    gfx_xbox_rapi_set_viewport,
    gfx_xbox_rapi_set_scissor,
    gfx_xbox_rapi_set_use_alpha,
    gfx_xbox_rapi_draw_triangles,
    gfx_xbox_rapi_init,
    gfx_xbox_rapi_on_resize,
    gfx_xbox_rapi_start_frame,
    gfx_xbox_rapi_end_frame,
    gfx_xbox_rapi_finish_render
};
#endif
