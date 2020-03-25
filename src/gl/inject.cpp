#include <iostream>
#include <array>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <string>
#include "real_dlsym.h"
#include "loaders/loader_gl.h"
#include "GL/gl3w.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "font_default.h"
#include "overlay.h"
#include "cpu.h"
#include "mesa/util/macros.h"
#include "mesa/util/os_time.h"
#include "file_utils.h"
#include "notify.h"
#include "dbus_info.h"

#include <chrono>
#include <iomanip>

#define EXPORT_C_(type) extern "C" __attribute__((__visibility__("default"))) type

EXPORT_C_(void *) glXGetProcAddress(const unsigned char* procName);
EXPORT_C_(void *) glXGetProcAddressARB(const unsigned char* procName);

static gl_loader gl;

struct state {
    ImGuiContext *imgui_ctx = nullptr;
    ImFont* font = nullptr;
    ImFont* font1 = nullptr;
};

struct GLVec
{
    GLint v[4];

    GLint operator[] (size_t i)
    {
        return v[i];
    }

    bool operator== (const GLVec& r)
    {
        return v[0] == r.v[0]
            && v[1] == r.v[1]
            && v[2] == r.v[2]
            && v[3] == r.v[3];
    }

    bool operator!= (const GLVec& r)
    {
        return !(*this == r);
    }
};

static GLVec last_vp {}, last_sb {};
static ImVec2 window_size;
static overlay_params params {};
static swapchain_stats sw_stats {};
static state state;
static notify_thread notifier;
static bool cfg_inited = false;
static bool inited = false;
static uint32_t vendorID;
static std::string deviceName;

// seems to quit by itself though
static std::unique_ptr<notify_thread, std::function<void(notify_thread *)>>
    stop_it(&notifier, [](notify_thread *n){ stop_notifier(*n); });

void imgui_init()
{
    if (cfg_inited)
        return;
    parse_overlay_config(&params, getenv("MANGOHUD_CONFIG"));
    notifier.params = &params;
    start_notifier(notifier);
    window_size = ImVec2(params.width, params.height);
    init_system_info();
    cfg_inited = true;
    init_cpu_stats(params);
    if (params.enabled[OVERLAY_PARAM_ENABLED_media_player]) {
        try {
            dbusmgr::dbus_mgr.init();
            get_spotify_metadata(dbusmgr::dbus_mgr, spotify);
        } catch (std::runtime_error& e) {
            std::cerr << "Failed to get initial Spotify metadata: " << e.what() << std::endl;
        }
    }
}

void imgui_create(void *ctx)
{
    if (inited)
        return;
    inited = true;

    if (!ctx)
        return;

    imgui_init();
    gl3wInit();

    std::cerr << "GL version: " << glGetString(GL_VERSION) << std::endl;
    glGetIntegerv(GL_MAJOR_VERSION, &sw_stats.version_gl.major);
    glGetIntegerv(GL_MINOR_VERSION, &sw_stats.version_gl.minor);

    deviceName = (char*)glGetString(GL_RENDERER);
    if (deviceName.find("Radeon") != std::string::npos
    || deviceName.find("AMD") != std::string::npos){
        vendorID = 0x1002;
    } else {
        vendorID = 0x10de;
    }
    init_gpu_stats(vendorID, params);
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    state.imgui_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();
    imgui_custom_style(params);

    glGetIntegerv (GL_VIEWPORT, last_vp.v);
    glGetIntegerv (GL_SCISSOR_BOX, last_sb.v);

    ImGui::GetIO().IniFilename = NULL;
    ImGui::GetIO().DisplaySize = ImVec2(last_vp[2], last_vp[3]);

    ImGui_ImplOpenGL3_Init();
    // Make a dummy GL call (we don't actually need the result)
    // IF YOU GET A CRASH HERE: it probably means that you haven't initialized the OpenGL function loader used by this code.
    // Desktop OpenGL 3/4 need a function loader. See the IMGUI_IMPL_OPENGL_LOADER_xxx explanation above.
    GLint current_texture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &current_texture);

    int font_size = params.font_size;
    if (!font_size)
        font_size = 24;

    if (!params.font_file.empty() && file_exists(params.font_file)) {
        state.font = io.Fonts->AddFontFromFileTTF(params.font_file.c_str(), font_size);
        state.font1 = io.Fonts->AddFontFromFileTTF(params.font_file.c_str(), font_size * 0.55f);
    } else {
        ImFontConfig font_cfg = ImFontConfig();
        const char* ttf_compressed_base85 = GetDefaultCompressedFontDataTTFBase85();
        const ImWchar* glyph_ranges = io.Fonts->GetGlyphRangesDefault();

        state.font = io.Fonts->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size, &font_cfg, glyph_ranges);
        state.font1 = io.Fonts->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size * 0.55, &font_cfg, glyph_ranges);
    }
    sw_stats.font1 = state.font1;
}

void imgui_shutdown()
{
#ifndef NDEBUG
    std::cerr << __func__ << std::endl;
#endif

    if (state.imgui_ctx) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext(state.imgui_ctx);
        state.imgui_ctx = nullptr;
    }
    inited = false;
}

void imgui_set_context(void *ctx)
{
    if (!ctx) {
        imgui_shutdown();
        return;
    }
#ifndef NDEBUG
    std::cerr << __func__ << ": " << ctx << std::endl;
#endif
    imgui_create(ctx);
}

void imgui_render()
{
    if (!ImGui::GetCurrentContext())
        return;

    // check which one is affected by window resize and use that
    GLVec vp; glGetIntegerv (GL_VIEWPORT, vp.v);
    GLVec sb; glGetIntegerv (GL_SCISSOR_BOX, sb.v);

    bool invalid_scissor = (sb[2] == 1 && sb[3] == 1);

    if (vp != last_vp || invalid_scissor) {
#ifndef NDEBUG
        printf("viewport: %d %d %d %d\n", vp[0], vp[1], vp[2], vp[3]);
#endif
        ImGui::GetIO().DisplaySize = ImVec2(vp[2], vp[3]);
    }

    if (!invalid_scissor &&
        (sb != last_sb
        || last_vp == sb) // openmw initial viewport size is the same (correct)
                         // at start as scissor box, so apply it instead
    ) {
#ifndef NDEBUG
        printf("scissor box: %d %d %d %d\n", sb[0], sb[1], sb[2], sb[3]);
#endif
        ImGui::GetIO().DisplaySize = ImVec2(sb[2], sb[3]);
    }

    last_vp = vp;
    last_sb = sb;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    {
        std::lock_guard<std::mutex> lk(notifier.mutex);
        position_layer(params, window_size);
        render_imgui(sw_stats, params, window_size, false);
    }
    ImGui::PopStyleVar(3);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void* get_proc_address(const char* name) {
    void (*func)() = (void (*)())real_dlsym( RTLD_NEXT, name );

    if (!func) {
        std::cerr << "MANGOHUD: Failed to get function '" << name << "'" << std::endl;
        exit( 1 );
    }

    return (void*)func;
}

void* get_glx_proc_address(const char* name) {
    if (!gl.Load()) {
        // Force load libGL then. If it still doesn't find it, get_proc_address should quit the program
        void *handle = real_dlopen("libGL.so.1", RTLD_LAZY);
        if (!handle)
            std::cerr << "MANGOHUD: couldn't find libGL.so.1" << std::endl;
        gl.Load(handle);
    }

    void *func = nullptr;
    if (gl.glXGetProcAddress)
        func = gl.glXGetProcAddress( (const unsigned char*) name );

    if (!func && gl.glXGetProcAddressARB)
        func = gl.glXGetProcAddressARB( (const unsigned char*) name );

    if (!func)
        func = get_proc_address( name );

    return func;
}

EXPORT_C_(void *) glXCreateContext(void *dpy, void *vis, void *shareList, int direct)
{
    gl.Load();
    void *ctx = gl.glXCreateContext(dpy, vis, shareList, direct);
#ifndef NDEBUG
    std::cerr << __func__ << ":" << ctx << std::endl;
#endif
    return ctx;
}

EXPORT_C_(bool) glXMakeCurrent(void* dpy, void* drawable, void* ctx) {
    gl.Load();
#ifndef NDEBUG
    std::cerr << __func__ << ": " << drawable << ", " << ctx << std::endl;
#endif

    bool ret = gl.glXMakeCurrent(dpy, drawable, ctx);
    if (ret)
        imgui_set_context(ctx);

    if (params.gl_vsync >= -1) {
        if (gl.glXSwapIntervalEXT)
            gl.glXSwapIntervalEXT(dpy, drawable, params.gl_vsync);
        if (gl.glXSwapIntervalSGI)
            gl.glXSwapIntervalSGI(params.gl_vsync);
        if (gl.glXSwapIntervalMESA)
            gl.glXSwapIntervalMESA(params.gl_vsync);
    }

    return ret;
}

EXPORT_C_(void) glXSwapBuffers(void* dpy, void* drawable) {
    gl.Load();
    imgui_create(gl.glXGetCurrentContext());
    check_keybinds(params);
    update_hud_info(sw_stats, params, vendorID);
    imgui_render();
    gl.glXSwapBuffers(dpy, drawable);
    if (fps_limit_stats.targetFrameTime > 0){
        fps_limit_stats.frameStart = os_time_get_nano();
        FpsLimiter(fps_limit_stats);
        fps_limit_stats.frameEnd = os_time_get_nano();
    }
}

EXPORT_C_(void) glXSwapIntervalEXT(void *dpy, void *draw, int interval) {
#ifndef NDEBUG
    std::cerr << __func__ << ": " << interval << std::endl;
#endif

    gl.Load();
    if (params.gl_vsync >= 0)
        interval = params.gl_vsync;
    gl.glXSwapIntervalEXT(dpy, draw, interval);
}

EXPORT_C_(int) glXSwapIntervalSGI(int interval) {
#ifndef NDEBUG
    std::cerr << __func__ << ": " << interval << std::endl;
#endif
    gl.Load();
    if (params.gl_vsync >= 0)
        interval = params.gl_vsync;
    return gl.glXSwapIntervalSGI(interval);
}

EXPORT_C_(int) glXSwapIntervalMESA(unsigned int interval) {
#ifndef NDEBUG
    std::cerr << __func__ << ": " << interval << std::endl;
#endif
    gl.Load();
    if (params.gl_vsync >= 0)
        interval = (unsigned int)params.gl_vsync;
    return gl.glXSwapIntervalMESA(interval);
}

EXPORT_C_(int) glXGetSwapIntervalMESA() {
    gl.Load();
    static bool first_call = true;
    int interval = gl.glXGetSwapIntervalMESA();

    if (first_call) {
        first_call = false;
        if (params.gl_vsync >= 0) {
            interval = params.gl_vsync;
            gl.glXSwapIntervalMESA(interval);
        }
    }

#ifndef NDEBUG
    std::cerr << __func__ << ": " << interval << std::endl;
#endif
    return interval;
}

struct func_ptr {
   const char *name;
   void *ptr;
};

static std::array<const func_ptr, 9> name_to_funcptr_map = {{
#define ADD_HOOK(fn) { #fn, (void *) fn }
   ADD_HOOK(glXGetProcAddress),
   ADD_HOOK(glXGetProcAddressARB),
   ADD_HOOK(glXCreateContext),
   ADD_HOOK(glXMakeCurrent),
   ADD_HOOK(glXSwapBuffers),

   ADD_HOOK(glXSwapIntervalEXT),
   ADD_HOOK(glXSwapIntervalSGI),
   ADD_HOOK(glXSwapIntervalMESA),
   ADD_HOOK(glXGetSwapIntervalMESA),
#undef ADD_HOOK
}};

static void *find_ptr(const char *name)
{
   for (auto& func : name_to_funcptr_map) {
      if (strcmp(name, func.name) == 0)
         return func.ptr;
   }

   return nullptr;
}

EXPORT_C_(void *) glXGetProcAddress(const unsigned char* procName) {
    //std::cerr << __func__ << ":" << procName << std::endl;

    void* func = find_ptr( (const char*)procName );
    if (func)
        return func;

    return get_glx_proc_address((const char*)procName);
}

EXPORT_C_(void *) glXGetProcAddressARB(const unsigned char* procName) {
    //std::cerr << __func__ << ":" << procName << std::endl;

    void* func = find_ptr( (const char*)procName );
    if (func)
        return func;

    return get_glx_proc_address((const char*)procName);
}

#ifdef HOOK_DLSYM
EXPORT_C_(void*) dlsym(void * handle, const char * name)
{
    void* func = find_ptr(name);
    if (func) {
        //fprintf(stderr,"%s: local: %s\n",  __func__ , name);
        return func;
    }

    //fprintf(stderr,"%s: foreign: %s\n",  __func__ , name);
    return real_dlsym(handle, name);
}
#endif