#if defined(TARGET_DC)

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#ifdef __MINGW32__
# define FOR_WINDOWS 1
#else
# define FOR_WINDOWS 0
#endif

#if FOR_WINDOWS || defined(OSX_BUILD)
# define GLEW_STATIC
# include <GL/glew.h>
#endif

#if defined(TARGET_DC)
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glext.h>
#include <GL/glkos.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#endif

#ifdef WAPI_SDL2
# include <SDL2/SDL.h>
# include <SDL2/SDL_opengl.h>
#elif defined(WAPI_SDL1)
# include <SDL/SDL.h>
# ifndef GLEW_STATIC
#  include <SDL/SDL_opengl.h>
# endif
#endif

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"
#include "gl_fast_vert.h"

enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;
};

struct SamplerState {
    GLenum min_filter;
    GLenum mag_filter;
    GLenum wrap_s;
    GLenum wrap_t;
    GLuint tex;
};

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader = NULL;

static struct SamplerState tmu_state[2];

static const dc_fast_t *cur_buf = NULL;
static bool gl_blend = false;
static bool gl_depth = false;
static bool gl_npot = false;

#if !defined(TARGET_DC)
static bool gl_multitexture = false;
static void *scale_buf = NULL;
static int scale_buf_size = 0;
#endif

/*
static float c_mix[] = { 0.f, 0.f, 0.f, 1.f };
static float c_invmix[] = { 1.f, 1.f, 1.f, 1.f };
static const float c_white[] = { 1.f, 1.f, 1.f, 1.f };
*/

static void resample_32bit(const uint32_t *in, const int inwidth, const int inheight, uint32_t *out, const int outwidth, const int outheight) {
  int i, j;
  const uint32_t *inrow;
  uint32_t frac, fracstep;

  fracstep = inwidth * 0x10000 / outwidth;
  for (i = 0; i < outheight; i++, out += outwidth) {
    inrow = in + inwidth * (i * inheight / outheight);
    frac = fracstep >> 1;
    for (j = 0; j < outwidth; j += 4) {
      out[j] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 1] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 2] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 3] = inrow[frac >> 16];
      frac += fracstep;
    }
  }
}

static void resample_16bit(const unsigned short *in, int inwidth, int inheight, unsigned short *out, int outwidth, int outheight) {
    int i, j;
    const unsigned short *inrow;
    unsigned int frac, fracstep;

    fracstep = inwidth * 0x10000 / outwidth;
    for (i = 0; i < outheight; i++, out += outwidth) {
        inrow = in + inwidth * (i * inheight / outheight);
        frac = fracstep >> 1;
        for (j = 0; j < outwidth; j += 4) {
            out[j] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 1] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 2] = inrow[frac >> 16];
            frac += fracstep;
            out[j + 3] = inrow[frac >> 16];
            frac += fracstep;
        }
    }
}

static inline uint32_t next_pot(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static inline uint32_t is_pot(const uint32_t v) {
    return (v & (v - 1)) == 0;
}

static bool gfx_opengl_z_is_from_0_to_1(void) {
    return true;
}

static inline GLenum texenv_set_color(UNUSED struct ShaderProgram *prg) {
    return GL_MODULATE;
}

static inline GLenum texenv_set_texture(UNUSED struct ShaderProgram *prg) {
    return GL_MODULATE;
}

static inline GLenum texenv_set_texture_color(struct ShaderProgram *prg) {
    GLenum mode;

    // HACK: lord forgive me for this, but this is easier
    switch (prg->shader_id) {
       // case 0x0000038D: // mario's eyes
        case 0x01045A00: // peach letter
        case 0x01200A00: // intro copyright fade in
            mode = GL_DECAL;
            break;
        case 0x00000551: // goddard
        /*@Note: Issues! */
            mode =  GL_REPLACE; /*GL_BLEND*/
            break;
        default:
            mode = GL_MODULATE;
            break;
    }

    return mode;
}

static inline GLenum texenv_set_texture_texture(UNUSED struct ShaderProgram *prg) {
    return GL_MODULATE;
}

static void gfx_opengl_apply_shader(struct ShaderProgram *prg) {
    // vertices are always there
    glVertexPointer(3, GL_FLOAT, sizeof(dc_fast_t), &cur_buf[0].vert);

    // have texture(s), specify same texcoords for every active texture
    if (prg->texture_used[0] || prg->texture_used[1]) {
        glEnable(GL_TEXTURE_2D);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, sizeof(dc_fast_t), &cur_buf[0].texture);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisable(GL_TEXTURE_2D);
    }

    /* Will be enabled when pvr fog is working, something isn't quite right current */
#if 0
    if (prg->shader_id & SHADER_OPT_FOG) {
        glDisable(GL_BLEND);
        glEnable(GL_FOG);
    } else {
        glDisable(GL_FOG);
    }
#endif

    if (prg->num_inputs) {
        // have colors
        // TODO: more than one color (maybe glSecondaryColorPointer?)
        // HACK: if there's a texture and two colors, one of them is likely for speculars or some shit (see mario head)
        //       if there's two colors but no texture, the real color is likely the second one
        // HACKHACK: alpha is 0 in the transition shader (0x01A00045), maybe figure out the flags instead
        //const int vlen = (prg->cc.opt_alpha && prg->shader_id != 0x01A00045) ? 4 : 3;
        //const int hack = vlen * (prg->num_inputs > 1);
        #if 0
        const int vlen = 4;
        const int hack = 0;


        if (prg->texture_used[1] && prg->cc.do_mix[0]) {
            // HACK: when two textures are mixed by vertex color, store the color
            //       it will be used later when rendering two texture passes
            c_mix[0] = *(ofs + hack + 0);
            c_mix[1] = *(ofs + hack + 1);
            c_mix[2] = *(ofs + hack + 2);
            c_invmix[0] = 1.f - c_mix[0];
            c_invmix[1] = 1.f - c_mix[1];
            c_invmix[2] = 1.f - c_mix[2];
            glDisableClientState(GL_COLOR_ARRAY);
            glColor3f(c_mix[0], c_mix[1], c_mix[2]);
        } else 
        #endif
        {
            // otherwise use vertex colors as normal
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, sizeof(dc_fast_t), &cur_buf[0].color);
        }
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
    }

    if (!prg->enabled) {
        // we only need to do this once
        prg->enabled = true;

        if (prg->shader_id & SHADER_OPT_TEXTURE_EDGE) {
            // (horrible) alpha discard
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 1.0f/3.0f);
        } else {
            glDisable(GL_ALPHA_TEST);
        }

        // configure texenv
        GLenum mode;
        switch (prg->mix) {
            case SH_MT_TEXTURE:         mode = texenv_set_texture(prg); break;
            case SH_MT_TEXTURE_TEXTURE: mode = texenv_set_texture_texture(prg); break;
            case SH_MT_TEXTURE_COLOR:   mode = texenv_set_texture_color(prg); break;
            default:                    mode = texenv_set_color(prg); break;
        }
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
    }
}

static void gfx_opengl_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_shader && (cur_shader == old_prg || !old_prg)) {
        cur_shader->enabled = false;
        cur_shader = NULL;
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    if (cur_shader)
        cur_shader->enabled = false;
}

static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];

    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used[0] = ccf.used_textures[0];
    prg->texture_used[1] = ccf.used_textures[1];

    if (ccf.used_textures[0] && ccf.used_textures[1]) {
        prg->mix = SH_MT_TEXTURE_TEXTURE;
        if (ccf.do_single[1]) {
            prg->texture_ord[0] = 1;
            prg->texture_ord[1] = 0;
        } else {
            prg->texture_ord[0] = 0;
            prg->texture_ord[1] = 1;
        }
    } else if (ccf.used_textures[0] && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (ccf.used_textures[0]) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    }

    prg->enabled = false;

    gfx_opengl_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_opengl_lookup_shader(uint32_t shader_id) {
    size_t i;

    for (i = 0; i < shader_program_pool_size; i++)
        if (shader_program_pool[i].shader_id == shader_id)
            return &shader_program_pool[i];
    return NULL;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->texture_used[0];
    used_textures[1] = prg->texture_used[1];
}

static uint32_t gfx_opengl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    return (uint32_t)ret;
}

static void gfx_opengl_select_texture(int tile, uint32_t texture_id) {
    tmu_state[tile].tex = texture_id; // remember this for multitexturing later
    glBindTexture(GL_TEXTURE_2D, texture_id);
}
/* Used for rescaling textures ROUGHLY into pow2 dims */
static unsigned int __attribute__((aligned(16))) scaled[64 * 64 * sizeof(unsigned int)]; /* 16kb */

//extern uint32_t pvr_mem_available(void);
static void gfx_opengl_upload_texture(const uint8_t *rgba32_buf, int width, int height, unsigned int type) {
    if (!gl_npot) {
        // we don't support non power of two textures, scale to next power of two if necessary
        if ((!is_pot(width) || !is_pot(height)) || (width < 8) || (height < 8)) {
            int pwidth = next_pot(width);
            int pheight = next_pot(height);
            /*@Note: Might not need texture max sizes */
            if(pwidth > 256){
                pwidth = 256;
            }
            if(pheight > 256){
                pheight = 256;
            }

            /* Need texture min sizes */
            if(pwidth < 8){
                pwidth = 8;
            }
            if(pheight < 8){
                pheight = 8;
            }
            if(type == GL_RGBA){
                resample_32bit((const uint32_t *)rgba32_buf, width, height, (uint32_t*)scaled, pwidth, pheight);
            } else {
                resample_16bit((const uint16_t *)rgba32_buf, width, height, (uint16_t*)scaled, pwidth, pheight);
            }
            rgba32_buf = (uint8_t*)scaled;
            width = pwidth;
            height = pheight;
        }
    }
    if(type == GL_RGBA){
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, rgba32_buf);
    }
#ifdef DEBUG
    printf("GL Mem left:%u\n", (unsigned int)pvr_mem_available());
#endif
}

static inline GLenum gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP) return GL_CLAMP;
   // return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    return  GL_REPEAT;
}

static inline void gfx_opengl_apply_tmu_state(const int tile) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tmu_state[tile].min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tmu_state[tile].mag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, tmu_state[tile].wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, tmu_state[tile].wrap_t);
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const GLenum filter = linear_filter ? GL_LINEAR : GL_NEAREST;

    const GLenum wrap_s = gfx_cm_to_opengl(cms);
    const GLenum wrap_t = gfx_cm_to_opengl(cmt);

    tmu_state[tile].min_filter = filter;
    tmu_state[tile].mag_filter = filter;
    tmu_state[tile].wrap_s = wrap_s;
    tmu_state[tile].wrap_t = wrap_t;

    // set state for the first texture right away
    if (!tile) gfx_opengl_apply_tmu_state(tile);
}

static void gfx_opengl_set_depth_test(bool depth_test) {
    if (depth_test)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
}

static void gfx_opengl_set_depth_mask(bool z_upd) {
    gl_depth = z_upd;
    glDepthMask(z_upd);
}

static bool is_zmode_decal = false;
// Polyoffset currently doesn't work so gotta workaround it. 
static void gfx_opengl_set_zmode_decal(bool zmode_decal) {
    is_zmode_decal = zmode_decal;
    if (zmode_decal) {
        glDepthFunc(GL_LEQUAL);  
    } else {
        glDepthFunc(GL_LESS);  
    }
}


static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha) {
    gl_blend = use_alpha;
    if (use_alpha)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
}

// draws the same triangles as plain fog color + fog intensity as alpha
// on top of the normal tris and blends them to achieve sort of the same effect
// as fog would
static inline void gfx_opengl_pass_fog(void) {
#ifndef TARGET_DC
    // if texturing is enabled, disable it, since we're blending colors
    if (cur_shader->texture_used[0] || cur_shader->texture_used[1])
        glDisable(GL_TEXTURE_2D);

    glEnableClientState(GL_COLOR_ARRAY); // enable color array temporarily
    glColorPointer(4, GL_FLOAT, cur_buf_stride, cur_fog_ofs); // set fog colors as primary colors
    if (!gl_blend) glEnable(GL_BLEND); // enable blending temporarily
    glDepthFunc(GL_LEQUAL); // Z is the same as the base triangles

    glDrawArrays(GL_TRIANGLES, 0, 3 * cur_buf_num_tris);

    glDepthFunc(GL_LESS); // set back to default
    if (!gl_blend) glDisable(GL_BLEND); // disable blending if it was disabled
    glDisableClientState(GL_COLOR_ARRAY); // will get reenabled later anyway

    // if texturing was enabled, re-enable it
    if (cur_shader->texture_used[0] || cur_shader->texture_used[1])
        glEnable(GL_TEXTURE_2D);
#endif
}

// this assumes the two textures are combined like so:
// result = mix(tex0.rgb, tex1.rgb, vertex.rgb)
static inline void gfx_opengl_pass_mix_texture(void) {
#ifndef TARGET_DC
    // set second texture
    glBindTexture(GL_TEXTURE_2D, tmu_state[cur_shader->texture_ord[1]].tex);
    gfx_opengl_apply_tmu_state(cur_shader->texture_ord[1]);

    if (!gl_blend) glEnable(GL_BLEND); // enable blending temporarily
    glBlendFunc(GL_ONE, GL_ONE); // additive blending
    glDepthFunc(GL_LEQUAL); // Z is the same as the base triangles

    // draw the same triangles, but with the inverse of the mix color
    glColor3f(c_invmix[0], c_invmix[1], c_invmix[2]);
    glDrawArrays(GL_TRIANGLES, 0, 3 * cur_buf_num_tris);
    glColor3f(1.f, 1.f, 1.f); // reset color

    glDepthFunc(GL_LESS); // set back to default
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // same here
    if (!gl_blend) glDisable(GL_BLEND); // disable blending if it was disabled

    // set old texture
    glBindTexture(GL_TEXTURE_2D, tmu_state[cur_shader->texture_ord[0]].tex);
    gfx_opengl_apply_tmu_state(cur_shader->texture_ord[0]);
#endif
}

static void gfx_opengl_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    cur_buf = (void*)buf_vbo;

    gfx_opengl_apply_shader(cur_shader);

    // if there's two textures, set primary texture first
    if (cur_shader->texture_used[1])
        glBindTexture(GL_TEXTURE_2D, tmu_state[cur_shader->texture_ord[0]].tex);
    
    if (cur_shader->shader_id == 0x0000038D) {
        // Face fix. 
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
        glEnable(GL_BLEND);
      //  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    
    /*@Error: Goddard hack */
    if(cur_shader->shader_id == 0x551){
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
    }

  if(cur_shader->shader_id == 18874437){ // 0x1200045, skybox  // may need to relook at this
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_BLEND);
        glDisable(GL_FOG);
        glPushMatrix();
        glLoadIdentity();
    }


  if (is_zmode_decal) {
        // Adjust depth values slightly for zmode_decal objects
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        
        // Push the geometry slightly towards the camera
        glPushMatrix();
        glTranslatef(0.0f, 2.1f, 0.9f);  // magic values need fine tuning. 
    }

    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);

    if (is_zmode_decal) {
        glPopMatrix();
        glDepthFunc(GL_LESS);  // Reset depth function
    }
    
    // pretty sure this is needed)
    if (cur_shader->shader_id == 0x0000038D) {
        glDisable(GL_BLEND);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }
    
    if(cur_shader->shader_id == 18874437){ // 0x1200045, skybox
        glPopMatrix();
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glEnable(GL_BLEND);
        glEnable(GL_FOG);
    }

    // if there's two textures, draw polys with the second texture
    //if (cur_shader->texture_used[1]) gfx_opengl_pass_mix_texture();

    // cur_fog_ofs is only set if GL_EXT_fog_coord isn't used
    //if (cur_fog_ofs) gfx_opengl_pass_fog();
}

extern void gfx_opengl_2d_projection(void);
extern void gfx_opengl_reset_projection(void);
void gfx_opengl_draw_triangles_2d(void *buf_vbo, UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    glDisable(GL_FOG);
    gfx_opengl_2d_projection();

    dc_fast_t *tris = buf_vbo;

    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(dc_fast_t), &tris[0].vert);
    glTexCoordPointer(2, GL_FLOAT, sizeof(dc_fast_t), &tris[0].texture);
    glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, sizeof(dc_fast_t), &tris[0].color);

    if(buf_vbo_num_tris) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        // if there's two textures, set primary texture first
        if (cur_shader->texture_used[1])
            glBindTexture(GL_TEXTURE_2D, tmu_state[cur_shader->texture_ord[0]].tex);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }

    glDrawArrays(GL_TRIANGLES, 0, 3 * 2 /* 2 tri quad */);
    gfx_opengl_reset_projection();
}

static inline bool gl_check_ext(const char *name) {
    static const char *extstr = NULL;

    if (extstr == NULL)
        extstr = (const char *)glGetString(GL_EXTENSIONS);

    if (!strstr(extstr, name)) {
        printf("GL extension not supported: %s\n", name);
        return false;
    }

    printf("GL extension detected: %s\n", name);
    return true;
}

static inline bool gl_get_version(int *major, int *minor, bool *is_es) {
    const char *vstr = (const char *)glGetString(GL_VERSION);
    if (!vstr || !vstr[0]) return false;

    if (!strncmp(vstr, "OpenGL ES ", 10)) {
        vstr += 10;
        *is_es = true;
    } else if (!strncmp(vstr, "OpenGL ES-CM ", 13)) {
        vstr += 13;
        *is_es = true;
    }

    return (sscanf(vstr, "%d.%d", major, minor) == 2);
}

#define sys_fatal printf

extern void getRamStatus(void);
static void gfx_opengl_init(void) {
#if FOR_WINDOWS || defined(OSX_BUILD)
    GLenum err;
    if ((err = glewInit()) != GLEW_OK)
        sys_fatal("could not init GLEW:\n%s", glewGetErrorString(err));
#endif

    GLdcConfig config;
    glKosInitConfig(&config);
    config.autosort_enabled = GL_TRUE;
    config.fsaa_enabled = GL_FALSE;
    /*@Note: These should be adjusted at some point */
    config.initial_op_capacity = 3584;
    config.initial_pt_capacity = 1024;
    config.initial_tr_capacity = 2048;
    config.initial_immediate_capacity = 0;
    glKosInitEx(&config);
    //glKosInit();

    getRamStatus();
    fflush(stdout);

    // check GL version
    int vmajor, vminor;
    bool is_es = false;
    gl_get_version(&vmajor, &vminor, &is_es);
    if ((vmajor < 2 && vminor < 1) || is_es)
        sys_fatal("OpenGL 1.1+ is required.\nReported version: %s%d.%d\n", is_es ? "ES" : "", vmajor, vminor);

#if !defined(TARGET_DC)
    // check if we support non power of two textures
    gl_npot = gl_check_ext("GL_ARB_texture_non_power_of_two");
    if (!gl_npot) {
        // don't support NPOT textures, prepare buffer for rescaling
        // this will be realloc'd as necessary
        scale_buf_size = 64 * 64 * 4;
        scale_buf = malloc(scale_buf_size);
        if (!scale_buf) sys_fatal("Out of memory allocating for NPOT scale buffer\n");
    }

    // check if we support multitexturing
    gl_multitexture = vmajor > 1 || vminor > 2 || gl_check_ext("GL_ARB_multitexture");
#endif

    printf("GL_VERSION = %s\n", glGetString(GL_VERSION));
    printf("GL_EXTENSIONS =\n%s\n", glGetString(GL_EXTENSIONS));

    // these also never change
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    // glDisable(GL_DITHER);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    //glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /*@Note: unsure */
    //glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c_white);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glShadeModel(GL_SMOOTH);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glViewport(0, 0, 640, 480);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* There seems to be hardware issues with fog, needs investigation */
    /* fog values
    BOB: 6400/59392 = 9.28/1 
    JRB: 1280/64512 and 1828/63964
    HMC: 3200/62592pvr
    */
    static float fog[4] = {1.f, 0.f, 0.f, 0.5f};
#if 1
    glFogi(GL_FOG_MODE,GL_LINEAR);
    glFogf(GL_FOG_START, 0.f);
    glFogf(GL_FOG_END, 256.f);
#else
    glFogi(GL_FOG_MODE,GL_EXP);
    fog[3] = ortho_z_far;
    glFogf(GL_FOG_DENSITY, ortho_z_near);
#endif

    glFogfv(GL_FOG_COLOR, fog);
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame(void) {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void gfx_opengl_end_frame(void) {
}

static void gfx_opengl_finish_render(void) {
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_z_is_from_0_to_1,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_test,
    gfx_opengl_set_depth_mask,
    gfx_opengl_set_zmode_decal,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render
};

#endif // RAPI_GL_LEGACY
