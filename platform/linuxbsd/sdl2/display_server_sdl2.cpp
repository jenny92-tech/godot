/**************************************************************************/
/*  display_server_sdl2.cpp                                               */
/**************************************************************************/
#ifdef SDL2_ENABLED

#include "display_server_sdl2.h"

#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/os/keyboard.h"
#include "input_remap.h"
#include "main/main.h"

// ── evdev (Linux raw input) ──────────────────────────────────────────
// linux/input.h gives us struct input_event + KEY_*/BTN_*/ABS_* codes.
// dirent + fcntl + unistd are for scanning /dev/input and reading the
// device file descriptors. None of this is SDL2 — it's the same path the
// kernel exposes to /any/ userspace consumer (SDL, libevdev, gptokeyb).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

// linux/input-event-codes.h #defines kernel key codes as plain macros:
//   #define KEY_0       11
//   #define KEY_1       2
//   ...
//   #define KEY_DELETE 111
// These collide with godot's Key enum members of the same names
// (Key::KEY_0 … Key::KEY_9, Key::KEY_DELETE) — the preprocessor sees
// `Key::KEY_0` after this include and substitutes `Key::11`, which is
// invalid C++ and breaks compilation deep inside _sdl_keycode_to_godot
// / _evdev_keycode_to_godot. We need the kernel codes at runtime to
// match incoming struct input_event::code values, so cache them as
// constexpr ints before undefining the macros — then both worlds work.
static constexpr int LINUX_KEY_0      = KEY_0;
static constexpr int LINUX_KEY_1      = KEY_1;
static constexpr int LINUX_KEY_2      = KEY_2;
static constexpr int LINUX_KEY_3      = KEY_3;
static constexpr int LINUX_KEY_4      = KEY_4;
static constexpr int LINUX_KEY_5      = KEY_5;
static constexpr int LINUX_KEY_6      = KEY_6;
static constexpr int LINUX_KEY_7      = KEY_7;
static constexpr int LINUX_KEY_8      = KEY_8;
static constexpr int LINUX_KEY_9      = KEY_9;
static constexpr int LINUX_KEY_DELETE = KEY_DELETE;
#undef KEY_0
#undef KEY_1
#undef KEY_2
#undef KEY_3
#undef KEY_4
#undef KEY_5
#undef KEY_6
#undef KEY_7
#undef KEY_8
#undef KEY_9
#undef KEY_DELETE

// 自己的 KMS+GBM+EGL 三件套 — 完全绕开 SDL2 video driver + godot EGLManager
#include "kms_config.h"
#include "kms_egl_context.h"
#include "kms_gbm_device.h"

// 注册 rasterizer 工厂(关键!没这个 godot RendererCompositor::create() 调
// _create_func = NULL → SIGSEGV。Wayland/X11 DSDL 都在自己构造里调这个,我们忘了)
#ifdef GLES3_ENABLED
#include "drivers/gles3/rasterizer_gles3.h"
#endif

// Full SDL2 header(SDL_Init / SDL_CreateWindow / SDL_Event 等)。
// SDL_VIDEODRIVER=dummy 时只用于 event/joystick,不调 GL/视频功能;
// 非 dummy(SDL-delegated GL 路径)时也用它建窗口 + GL context。
#include <SDL2/SDL.h>

// glad EGL 函数指针表加载器(定义在 godot thirdparty/glad,kms_egl_context.cpp
// 也这么取)。SDL-delegated GL 路径里调它填 godot 的 eglGetProcAddress 指针,
// 让 rasterizer_gles3 用 eglGetProcAddress 把 GL 函数加载进 SDL 的当前 context。
// EGLDisplay 实质是 void*,这里用 void* 匹配 C linkage 签名,免 include EGL 头。
extern "C" int gladLoaderLoadEGL(void *display);

// ===================================================================
// register / create
// ===================================================================
void DisplayServerSDL2::register_sdl2_driver() {
	register_create_function("sdl2", create_func, get_rendering_drivers_func);
}

DisplayServer *DisplayServerSDL2::create_func(const String &p_rendering_driver, WindowMode p_mode,
		VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position,
		const Vector2i &p_resolution, int p_screen, Context p_context,
		int64_t p_parent_window, Error &r_error) {
	DisplayServer *ds = memnew(DisplayServerSDL2(p_rendering_driver, p_mode, p_vsync_mode, p_flags, p_resolution, p_context, r_error));
	if (r_error != OK) {
		ERR_PRINT("DisplayServerSDL2: failed to create display");
		if (ds) {
			memdelete(ds);
		}
		return nullptr;
	}
	return ds;
}

Vector<String> DisplayServerSDL2::get_rendering_drivers_func() {
	Vector<String> drivers;
#ifdef GLES3_ENABLED
	drivers.push_back("opengl3");
#endif
	return drivers;
}

// ===================================================================
// constructor / destructor
// ===================================================================
DisplayServerSDL2::DisplayServerSDL2(const String &p_rendering_driver, WindowMode p_mode,
		VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i &p_resolution,
		Context p_context, Error &r_error) {
	r_error = OK;
	rendering_driver = p_rendering_driver;
	vsync_mode = p_vsync_mode;
	window_flags = p_flags;

	// === 第零阶段:读 env config + 打 dump ===
	KMSConfig::load_from_env();
	KMSConfig::dump();
	poc_diag("DSDL2: ctor entered");

	// === 第一阶段:SDL2 init ===
	poc_diag("DSDL2: SDL_Init(VIDEO|EVENTS|JOYSTICK|AUDIO)");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO) != 0) {
		poc_diag_fmt("DSDL2: SDL_Init FAILED: %s", SDL_GetError());
		ERR_PRINT(vformat("DisplayServerSDL2: SDL_Init failed: %s", SDL_GetError()));
		r_error = ERR_UNAVAILABLE;
		return;
	}
	const char *vd = SDL_GetCurrentVideoDriver();
	poc_diag_fmt("DSDL2: SDL2 video driver = %s", vd ? vd : "(null)");

	// ── SDL-delegated GL path ───────────────────────────────────────────
	// 真 video driver(非 dummy)→ SDL 自管窗口 + GL context,我们只 SwapWindow。
	// 自包含:成功就在这里 return,完全不进下面的自写 KMS 路径(TrimUI dummy 不受影响)。
	use_sdl_gl = (vd != nullptr && String::utf8(vd) != "dummy");
	if (use_sdl_gl) {
#ifdef GLES3_ENABLED
		poc_diag_fmt("DSDL2: SDL-delegated GL 模式 (video driver '%s') — SDL 管窗口+GL context", vd);

		// unityloader(HK/黑神话)在同款 Mali 上验证过的属性:GLES profile + 3.x。
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

		// 0×0 + FULLSCREEN_DESKTOP = 取合成器/输出原生尺寸(weston 已 rotate-90)。
		poc_diag("DSDL2: SDL_CreateWindow(OPENGL|FULLSCREEN_DESKTOP)");
		window = SDL_CreateWindow("godot", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
				0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
		if (window == nullptr) {
			poc_diag_fmt("DSDL2: SDL_CreateWindow(OPENGL) FAILED: %s", SDL_GetError());
			ERR_PRINT(vformat("DisplayServerSDL2: SDL_CreateWindow(OPENGL) failed: %s", SDL_GetError()));
			SDL_Quit();
			r_error = ERR_UNAVAILABLE;
			return;
		}

		// GLES 3.2 → 3.1 → 3.0 逐级回退(libmali 普遍 3.x,版本因芯片而异)。
		SDL_GLContext glc = SDL_GL_CreateContext(window);
		if (glc == nullptr) {
			poc_diag_fmt("DSDL2: GLES 3.2 context 失败 (%s),回退 3.1", SDL_GetError());
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
			glc = SDL_GL_CreateContext(window);
		}
		if (glc == nullptr) {
			poc_diag_fmt("DSDL2: GLES 3.1 context 失败 (%s),回退 3.0", SDL_GetError());
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
			glc = SDL_GL_CreateContext(window);
		}
		if (glc == nullptr) {
			poc_diag_fmt("DSDL2: SDL_GL_CreateContext FAILED: %s", SDL_GetError());
			ERR_PRINT(vformat("DisplayServerSDL2: SDL_GL_CreateContext failed: %s", SDL_GetError()));
			SDL_DestroyWindow(window);
			window = nullptr;
			SDL_Quit();
			r_error = ERR_UNAVAILABLE;
			return;
		}
		sdl_gl_context = (void *)glc;
		SDL_GL_MakeCurrent(window, glc);

		// 填 godot glad 的 eglGetProcAddress 指针表,使 rasterizer_gles3 的
		// has_egl 分支(gladLoadGLES2 via eglGetProcAddress)命中,把 GL 加载进
		// SDL 的当前 context。NO_DISPLAY 足够拿到全局 eglGetProcAddress。
		poc_diag("DSDL2: gladLoaderLoadEGL(NO_DISPLAY) — 填 glad EGL 指针");
		gladLoaderLoadEGL(nullptr);

		SDL_GL_SetSwapInterval(vsync_mode == VSYNC_ENABLED ? 1 : 0);

		int dw = 0, dh = 0;
		SDL_GL_GetDrawableSize(window, &dw, &dh);
		// wayland: 刚建窗合成器尚未 configure,GetDrawableSize 常返回 1×1(占位)。
		// 把 ≤1 视为"尚未就绪",回落到 launcher 传入的 --resolution(=合成器逻辑尺寸)。
		// 真正的尺寸会随首个 SDL_WINDOWEVENT_SIZE_CHANGED 到达,在事件处理里被接受。
		window_size = (dw > 1 && dh > 1)
				? Size2i(dw, dh)
				: Size2i(p_resolution.width > 0 ? p_resolution.width : 1280,
						  p_resolution.height > 0 ? p_resolution.height : 720);
		panel_rotation = 0; // 旋转由 weston 负责,godot 不转
		poc_diag_fmt("DSDL2: SDL-GL window %dx%d, swap=SDL_GL_SwapWindow", window_size.width, window_size.height);

		window_mode = WINDOW_MODE_FULLSCREEN;
		window_visible = true;

		// 注册 GLES rasterizer 工厂(与 KMS 路径同,gles_over_gl=false)。
		poc_diag("DSDL2: RasterizerGLES3::make_current(false)");
		RasterizerGLES3::make_current(false);

		show_window(MAIN_WINDOW_ID);
		Input::get_singleton()->set_event_dispatch_function(_dispatch_input_events);
		// 输入仍走 evdev 单一来源(见 _process_sdl_event 里 use_sdl_gl 的门控,
		// 避免 SDL 事件 + evdev 双份)。
		_scan_evdev();
		poc_diag("DSDL2: ===== SDL-delegated GL CONSTRUCTOR COMPLETE =====");
		print_verbose(vformat("DisplayServerSDL2: SDL-delegated GL ready (%s), %dx%d", vd, window_size.width, window_size.height));
		return;
#else
		ERR_PRINT("DisplayServerSDL2: SDL-delegated GL 需要 GLES3_ENABLED");
		SDL_Quit();
		r_error = ERR_UNAVAILABLE;
		return;
#endif
	}

	poc_diag("DSDL2: SDL_CreateWindow(HIDDEN)");
	window = SDL_CreateWindow("godot", 0, 0,
			p_resolution.width > 0 ? p_resolution.width : 1280,
			p_resolution.height > 0 ? p_resolution.height : 720,
			SDL_WINDOW_HIDDEN);
	poc_diag_fmt("DSDL2: SDL_Window=%p", (void *)window);

	// === SKIP_KMS:不开 KMS/EGL,只 SDL2 dummy,测 godot 本身在 dummy-only 下崩不崩 ===
	if (KMSConfig::skip_kms) {
		poc_diag("DSDL2: POC_SKIP_KMS=1, 不开 KMS+GBM+EGL — godot 在无 GL context 下大概率 crash,只为 bisect");
		window_size = p_resolution.width > 0 ? p_resolution : Size2i(1280, 720);
		window_mode = WINDOW_MODE_FULLSCREEN;
		window_visible = true;
		return;
	}

	// === 第二阶段:自己开 /dev/dri/card0 + GBM ===
	poc_diag("DSDL2: memnew(KMSGBMDevice)");
	kms_dev = memnew(KMSGBMDevice());
	poc_diag("DSDL2: kms_dev->initialize()");
	if (kms_dev->initialize() != OK) {
		poc_diag("DSDL2: KMSGBMDevice::initialize FAILED");
		ERR_PRINT("DisplayServerSDL2: KMSGBMDevice::initialize 失败");
		memdelete(kms_dev);
		kms_dev = nullptr;
		if (window) {
			SDL_DestroyWindow(window);
			window = nullptr;
		}
		SDL_Quit();
		r_error = ERR_UNAVAILABLE;
		return;
	}
	poc_diag("DSDL2: KMSGBMDevice ready");

	// Authoritative panel size.
	//
	// Default: the real KMSDRM connector mode width/height — what the
	// hardware reports. Universal: TrimUI / MiniLoong / RG35XX / any
	// future ARM handheld using our DSDL2 backend gets the correct
	// value automatically because DRM knows the panel.
	//
	// Override hooks (all optional, port-side via launcher.sh env):
	//   GODOT_SDL2_PANEL_WIDTH  — force panel width (any positive int)
	//   GODOT_SDL2_PANEL_HEIGHT — force panel height (any positive int)
	//   GODOT_SDL2_ROTATION     — store rotation degrees, 0/90/180/270
	//                             (we cache the value; actual rotation
	//                             application is per-device future work)
	//
	// Why env, not a config file: launcher.sh already knows which port
	// it is and which device it ships on. No extra IO path, no path
	// resolution, no priority rules. The port author sets one or two
	// env lines in their launcher.sh; everyone else gets DRM defaults.
	{
		int drm_w = kms_dev->get_mode_width();
		int drm_h = kms_dev->get_mode_height();
		int panel_w = drm_w;
		int panel_h = drm_h;
		const char *env_w = getenv("GODOT_SDL2_PANEL_WIDTH");
		const char *env_h = getenv("GODOT_SDL2_PANEL_HEIGHT");
		const char *env_r = getenv("GODOT_SDL2_ROTATION");
		if (env_w != nullptr) {
			int v = atoi(env_w);
			if (v > 0) {
				panel_w = v;
			}
		}
		if (env_h != nullptr) {
			int v = atoi(env_h);
			if (v > 0) {
				panel_h = v;
			}
		}
		panel_rotation = 0;
		if (env_r != nullptr) {
			int v = atoi(env_r);
			if (v == 0 || v == 90 || v == 180 || v == 270) {
				panel_rotation = v;
			}
		}
		// On a 90°/270° panel the framebuffer stays physical (panel_w x panel_h),
		// but godot's logical screen is the SWAPPED landscape size so the game renders
		// landscape into the RT; RasterizerGLES3 rotates it onto the panel at blit time.
		if (panel_rotation == 90 || panel_rotation == 270) {
			window_size = Size2i(panel_h, panel_w);
		} else {
			window_size = Size2i(panel_w, panel_h);
		}
		poc_diag_fmt("DSDL2: panel %dx%d (DRM=%dx%d, env_w=%s env_h=%s env_r=%s) rotation=%d",
				panel_w, panel_h, drm_w, drm_h,
				env_w ? env_w : "<unset>",
				env_h ? env_h : "<unset>",
				env_r ? env_r : "<unset>",
				panel_rotation);
	}

	// === 第三阶段:自家最小化 EGL bootstrap ===
#ifdef GLES3_ENABLED
	poc_diag("DSDL2: memnew(KMSEGLContext)");
	egl_ctx = memnew(KMSEGLContext());
	poc_diag("DSDL2: egl_ctx->initialize()");
	if (egl_ctx->initialize((void *)kms_dev->get_gbm_device(), (void *)kms_dev->get_gbm_surface()) != OK) {
		poc_diag("DSDL2: KMSEGLContext::initialize FAILED");
		ERR_PRINT("DisplayServerSDL2: KMSEGLContext::initialize 失败");
		memdelete(egl_ctx);
		egl_ctx = nullptr;
		memdelete(kms_dev);
		kms_dev = nullptr;
		if (window) {
			SDL_DestroyWindow(window);
			window = nullptr;
		}
		SDL_Quit();
		r_error = ERR_UNAVAILABLE;
		return;
	}
	poc_diag("DSDL2: KMSEGLContext ready");

	egl_ctx->set_vsync(vsync_mode == VSYNC_ENABLED);
#endif

	window_mode = WINDOW_MODE_FULLSCREEN;
	window_visible = true;

#ifdef GLES3_ENABLED
	// 关键:注册 RasterizerGLES3 工厂(gles_over_gl=false → GLES 不是桌面 GL)。
	// 不调这个 → RendererCompositor::_create_func 是 NULL → RenderingServer::init 调它 NULL deref 死。
	// Wayland/X11 DSDL 都在自己构造里按 driver 分支调,我们没 driver 分支,直接调 GLES 版本。
	poc_diag("DSDL2: RasterizerGLES3::make_current(false) — 注册 GLES rasterizer 工厂");
	RasterizerGLES3::make_current(false);
#endif

	// 显式 show_window(MAIN_WINDOW_ID)对齐 wayland 构造尾部行为(主窗口 visible 标记 + SDL_ShowWindow)。
	// godot setup2 本会调,但仅条件 has_feature(SUBWINDOWS)=true 时;我们 SUBWINDOWS 返 false。
	poc_diag("DSDL2: show_window(MAIN_WINDOW_ID)");
	show_window(MAIN_WINDOW_ID);

	// Register the input event dispatch thunk. This is the missing link
	// that makes Input::parse_input_event() events actually reach the
	// SceneTree (and from there script _input(event) callbacks). Every
	// other godot DisplayServer does this; without it the gamepad event
	// stream we'd just built was being parsed for state but never
	// dispatched to the game.
	Input::get_singleton()->set_event_dispatch_function(_dispatch_input_events);

	// Open /dev/input/event* directly — SDL2 won't help us here because we
	// disabled its video subsystem (dummy / KMSDRM conflict), which also
	// disables its keyboard probing. _scan_evdev() opens every device,
	// classifies as keyboard/joystick, and registers joypads with godot's
	// Input singleton so signals fire from the first button press.
	_scan_evdev();

	poc_diag("DSDL2: ============== CONSTRUCTOR COMPLETE,godot 接管渲染 ==============");
	print_verbose(vformat("DisplayServerSDL2: KMS+GBM+EGL ready, %dx%d", window_size.width, window_size.height));
}

DisplayServerSDL2::~DisplayServerSDL2() {
	_close_evdev();
#ifdef GLES3_ENABLED
	if (egl_ctx) {
		memdelete(egl_ctx);
		egl_ctx = nullptr;
	}
#endif
	if (kms_dev) {
		memdelete(kms_dev);
		kms_dev = nullptr;
	}
	if (sdl_gl_context) {
		SDL_GL_DeleteContext((SDL_GLContext)sdl_gl_context);
		sdl_gl_context = nullptr;
	}
	if (window) {
		SDL_DestroyWindow(window);
		window = nullptr;
	}
	SDL_Quit();
}

// ===================================================================
// real impls (the ones godot main loop actually calls)
// ===================================================================
bool DisplayServerSDL2::has_feature(Feature p_feature) const {
	switch (p_feature) {
		case FEATURE_SUBWINDOWS:
		case FEATURE_IME:
		case FEATURE_CLIPBOARD_PRIMARY:
			return false;
		case FEATURE_MOUSE:
		case FEATURE_KEEP_SCREEN_ON:
		case FEATURE_SWAP_BUFFERS:
			return true;
		default:
			return false;
	}
}

String DisplayServerSDL2::get_name() const { return "SDL2"; }
int DisplayServerSDL2::get_screen_count() const { return 1; }
int DisplayServerSDL2::get_primary_screen() const { return 0; }

Vector<DisplayServer::WindowID> DisplayServerSDL2::get_window_list() const {
	Vector<WindowID> v;
	v.push_back(MAIN_WINDOW_ID);
	return v;
}

DisplayServer::WindowID DisplayServerSDL2::get_window_at_screen_position(const Point2i &) const {
	return MAIN_WINDOW_ID;
}

void DisplayServerSDL2::window_set_title(const String &p_title, WindowID) {
	if (window) {
		SDL_SetWindowTitle(window, p_title.utf8().get_data());
	}
}

void DisplayServerSDL2::window_set_size(const Size2i p_size, WindowID) {
	// KMSDRM 一次定 mode 就固定了(panel 物理分辨率),resize 实际无意义;静默忽略请求。
	(void)p_size;
}

Size2i DisplayServerSDL2::window_get_size(WindowID) const { return window_size; }
Size2i DisplayServerSDL2::window_get_size_with_decorations(WindowID) const { return window_size; }

void DisplayServerSDL2::window_set_position(const Point2i &p_position, WindowID) {
	if (window) {
		SDL_SetWindowPosition(window, p_position.x, p_position.y);
		window_position = p_position;
	}
}

Point2i DisplayServerSDL2::window_get_position(WindowID) const { return window_position; }
Point2i DisplayServerSDL2::window_get_position_with_decorations(WindowID) const { return window_position; }

void DisplayServerSDL2::window_set_mode(WindowMode p_mode, WindowID) {
	if (!window) return;
	uint32_t flag = 0;
	if (p_mode == WINDOW_MODE_FULLSCREEN || p_mode == WINDOW_MODE_EXCLUSIVE_FULLSCREEN) {
		flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
	}
	SDL_SetWindowFullscreen(window, flag);
	window_mode = p_mode;
}

DisplayServer::WindowMode DisplayServerSDL2::window_get_mode(WindowID) const { return window_mode; }

void DisplayServerSDL2::window_set_vsync_mode(VSyncMode p_vsync_mode, WindowID) {
#ifdef GLES3_ENABLED
	if (use_sdl_gl) {
		SDL_GL_SetSwapInterval(p_vsync_mode == VSYNC_ENABLED ? 1 : 0);
	} else if (egl_ctx) {
		egl_ctx->set_vsync(p_vsync_mode == VSYNC_ENABLED);
	}
#endif
	vsync_mode = p_vsync_mode;
}

DisplayServer::VSyncMode DisplayServerSDL2::window_get_vsync_mode(WindowID) const { return vsync_mode; }

void DisplayServerSDL2::window_attach_instance_id(ObjectID p_instance, WindowID) { window_attached_instance_id = p_instance; }
ObjectID DisplayServerSDL2::window_get_attached_instance_id(WindowID) const { return window_attached_instance_id; }
void DisplayServerSDL2::window_set_rect_changed_callback(const Callable &c, WindowID) { rect_changed_callback = c; }
void DisplayServerSDL2::window_set_window_event_callback(const Callable &c, WindowID) { window_event_callback = c; }
void DisplayServerSDL2::window_set_input_event_callback(const Callable &c, WindowID) { input_event_callback = c; }
void DisplayServerSDL2::window_set_input_text_callback(const Callable &c, WindowID) { input_text_callback = c; }
void DisplayServerSDL2::window_set_drop_files_callback(const Callable &c, WindowID) { drop_files_callback = c; }

void DisplayServerSDL2::window_set_current_screen(int, WindowID) {}
int DisplayServerSDL2::window_get_current_screen(WindowID) const { return 0; }

void DisplayServerSDL2::window_set_flag(WindowFlags p_flag, bool p_enabled, WindowID) {
	if (p_enabled) {
		window_flags |= (1 << p_flag);
	} else {
		window_flags &= ~(1 << p_flag);
	}
}

bool DisplayServerSDL2::window_get_flag(WindowFlags p_flag, WindowID) const {
	return (window_flags & (1 << p_flag)) != 0;
}

bool DisplayServerSDL2::can_any_window_draw() const { return window_visible && window != nullptr; }

void DisplayServerSDL2::swap_buffers() {
#ifdef GLES3_ENABLED
	if (use_sdl_gl) {
		if (window) {
			SDL_GL_SwapWindow(window);
		}
		return;
	}
	if (egl_ctx) {
		egl_ctx->swap_buffers();
	}
	if (kms_dev) {
		kms_dev->page_flip();
	}
#endif
}

void DisplayServerSDL2::show_window(WindowID) {
	if (window) SDL_ShowWindow(window);
	window_visible = true;
}

// ===================================================================
// SDL_PollEvent → godot input event
// ===================================================================
void DisplayServerSDL2::_dispatch_event(const Ref<InputEvent> &p_event) {
	if (input_event_callback.is_valid()) {
		Variant ev = p_event;
		const Variant *args[1] = { &ev };
		Variant ret;
		Callable::CallError err;
		input_event_callback.callp(args, 1, ret, err);
	}
}

// Static thunk for Input::set_event_dispatch_function. godot's Input
// singleton calls this for every event that lands in parse_input_event
// (keyboard, joypad, mouse, anything that goes through buffered_events
// + flush). We route it through the live DisplayServerSDL2 singleton's
// _dispatch_event so the input_event_callback the engine wired in via
// DisplayServer::window_set_input_event_callback ends up firing — and
// from there the SceneTree fans out to script _input(event) callbacks.
//
// X11 / Wayland / Windows / Android / Web all do this exact dance from
// their respective constructors; we were missing the registration,
// which is why joy_button events sat unparsed until we added the
// explicit parse_input_event call — and even then they never reached
// scripts because event_dispatch_function was still null.
void DisplayServerSDL2::_dispatch_input_events(const Ref<InputEvent> &p_event) {
	DisplayServerSDL2 *ds = static_cast<DisplayServerSDL2 *>(get_singleton());
	if (ds) {
		ds->_dispatch_event(p_event);
	}
}

// Map SDL2 keycode → godot Key enum.
//
// This translation has to live here because godot's main DisplayServer event
// loop only fans out keys it actually recognises; anything that returns
// Key::NONE here gets dropped before reaching Input::parse_input_event(),
// the game scripts, and the UI navigation actions (ui_accept / ui_up / …).
//
// What's covered (please keep this list in sync with the switch below):
//   * ASCII letters a-z         (lowercase)            via range cast
//   * Digits 0-9                                       via range cast
//   * Control keys: Enter, Escape, Backspace, Tab, Space
//   * Arrow keys: Up, Down, Left, Right
//   * Modifiers: Shift, Ctrl, Alt, Meta/Super, AltGr
//   * Navigation: Home, End, PageUp, PageDown, Insert, Delete
//   * Function keys: F1-F12
//   * Numpad: digits 0-9, Enter, +, -, *, /, ., =
//   * Symbol keys: backquote/grave, minus, equal, brackets, backslash,
//     semicolon, apostrophe, comma, period, slash
//
// What's NOT covered (Key::NONE → silently dropped — add here if needed):
//   * Media keys (volume / brightness / play-pause)
//   * Capslock / Numlock / Scrollock (we don't model lock state yet)
//   * Print, Pause, Menu (rare on handheld)
//   * F13+ function keys
//   * SDL_TEXTINPUT for proper text input (handled separately when added)
//   * Mobile/Android-specific keys (back button, etc.)
//
// gptokeyb default mapping sends a narrow subset (A->Enter, B->Esc, DPad
// arrows, etc.), so this map is sufficient for the common handheld path,
// but it's worth keeping broader-than-strictly-needed so a user with a
// real USB keyboard can also drive in-engine text fields / debug consoles.
static Key _sdl_keycode_to_godot(SDL_Keycode sym) {
	// SDLK values for printable ASCII match the character codes, and
	// godot's Key enum is laid out the same way for [0-9] and [a-z].
	// Direct cast is cheaper than a 36-arm switch.
	if (sym >= SDLK_a && sym <= SDLK_z) {
		return (Key)((int)Key::A + (sym - SDLK_a));
	}
	if (sym >= SDLK_0 && sym <= SDLK_9) {
		return (Key)((int)Key::KEY_0 + (sym - SDLK_0));
	}

	switch (sym) {
		// ── Control / whitespace ──────────────────────────────────────
		case SDLK_RETURN:    return Key::ENTER;     // gptokeyb A → Enter (ui_accept)
		case SDLK_ESCAPE:    return Key::ESCAPE;    // gptokeyb B → Esc   (ui_cancel)
		case SDLK_BACKSPACE: return Key::BACKSPACE;
		case SDLK_TAB:       return Key::TAB;
		case SDLK_SPACE:     return Key::SPACE;     // also ui_accept by default

		// ── Arrow keys ────────────────────────────────────────────────
		case SDLK_UP:        return Key::UP;        // gptokeyb D-pad ↑
		case SDLK_DOWN:      return Key::DOWN;
		case SDLK_LEFT:      return Key::LEFT;
		case SDLK_RIGHT:     return Key::RIGHT;

		// ── Modifiers ─────────────────────────────────────────────────
		case SDLK_LSHIFT:
		case SDLK_RSHIFT:    return Key::SHIFT;
		case SDLK_LCTRL:
		case SDLK_RCTRL:     return Key::CTRL;
		case SDLK_LALT:      return Key::ALT;
		case SDLK_RALT:      return Key::ALT;       // we don't separate AltGr
		case SDLK_LGUI:
		case SDLK_RGUI:      return Key::META;

		// ── Navigation ────────────────────────────────────────────────
		case SDLK_HOME:      return Key::HOME;
		case SDLK_END:       return Key::END;
		case SDLK_PAGEUP:    return Key::PAGEUP;
		case SDLK_PAGEDOWN:  return Key::PAGEDOWN;
		case SDLK_INSERT:    return Key::INSERT;
		case SDLK_DELETE:    return Key::KEY_DELETE;

		// ── Function keys F1-F12 ─────────────────────────────────────
		case SDLK_F1:  return Key::F1;
		case SDLK_F2:  return Key::F2;
		case SDLK_F3:  return Key::F3;
		case SDLK_F4:  return Key::F4;
		case SDLK_F5:  return Key::F5;
		case SDLK_F6:  return Key::F6;
		case SDLK_F7:  return Key::F7;
		case SDLK_F8:  return Key::F8;
		case SDLK_F9:  return Key::F9;
		case SDLK_F10: return Key::F10;
		case SDLK_F11: return Key::F11;
		case SDLK_F12: return Key::F12;

		// ── Numpad (some gptokeyb configs route to numpad) ──────────
		case SDLK_KP_0: return Key::KP_0;
		case SDLK_KP_1: return Key::KP_1;
		case SDLK_KP_2: return Key::KP_2;
		case SDLK_KP_3: return Key::KP_3;
		case SDLK_KP_4: return Key::KP_4;
		case SDLK_KP_5: return Key::KP_5;
		case SDLK_KP_6: return Key::KP_6;
		case SDLK_KP_7: return Key::KP_7;
		case SDLK_KP_8: return Key::KP_8;
		case SDLK_KP_9: return Key::KP_9;
		case SDLK_KP_ENTER:    return Key::KP_ENTER;
		case SDLK_KP_PLUS:     return Key::KP_ADD;
		case SDLK_KP_MINUS:    return Key::KP_SUBTRACT;
		case SDLK_KP_MULTIPLY: return Key::KP_MULTIPLY;
		case SDLK_KP_DIVIDE:   return Key::KP_DIVIDE;
		case SDLK_KP_PERIOD:   return Key::KP_PERIOD;

		// ── Symbol keys (USB keyboard text entry) ────────────────────
		case SDLK_BACKQUOTE:    return Key::QUOTELEFT;   // `~
		case SDLK_MINUS:        return Key::MINUS;
		case SDLK_EQUALS:       return Key::EQUAL;
		case SDLK_LEFTBRACKET:  return Key::BRACKETLEFT;
		case SDLK_RIGHTBRACKET: return Key::BRACKETRIGHT;
		case SDLK_BACKSLASH:    return Key::BACKSLASH;
		case SDLK_SEMICOLON:    return Key::SEMICOLON;
		case SDLK_QUOTE:        return Key::APOSTROPHE;
		case SDLK_COMMA:        return Key::COMMA;
		case SDLK_PERIOD:       return Key::PERIOD;
		case SDLK_SLASH:        return Key::SLASH;

		default:
			// Anything we didn't map = dropped. Keys most likely to land
			// here in practice: media keys, lock keys, F13+. If a deployed
			// gptokeyb config routes a button to one of these, add the
			// case above — silent NONE means an unresponsive UI.
			//
			// Logging the dropped key (with its SDL name when available)
			// is the only way to spot a gap in this table during a real
			// handheld test session — without it the symptom is "button
			// X just doesn't work" and we have no hint why.
			print_verbose(vformat(
					"DisplayServerSDL2: dropped SDL key (no godot Key mapping). "
					"SDLK code=0x%x name=%s",
					(int)sym, String(SDL_GetKeyName(sym))));
			return Key::NONE;
	}
}

// SDL_GameController is SDL2's normalized gamepad layout. The button enum
// values match godot's JoyButton 1:1 because both are SDL_GameController
// derived — A=0, B=1, X=2, Y=3, etc. Forward as-is and let godot's Input
// do the action mapping.
//
// NOTE this path is only reached for events SDL2 actually receives. With
// SDL_VIDEODRIVER=dummy SDL doesn't probe evdev keyboards/gamepads, so
// in practice every gamepad event arrives via _process_evdev() below
// instead — this function stays in case a future configuration enables
// a real SDL2 video driver where SDL DOES surface controller events.
static JoyButton _sdl_controller_button_to_godot(SDL_GameControllerButton b) {
	if (b < SDL_CONTROLLER_BUTTON_A || b >= SDL_CONTROLLER_BUTTON_MAX) {
		return JoyButton::INVALID;
	}
	return (JoyButton)(int)b;
}

static JoyAxis _sdl_controller_axis_to_godot(SDL_GameControllerAxis a) {
	if (a < SDL_CONTROLLER_AXIS_LEFTX || a >= SDL_CONTROLLER_AXIS_MAX) {
		return JoyAxis::INVALID;
	}
	return (JoyAxis)(int)a;
}

void DisplayServerSDL2::_process_sdl_event(const SDL_Event &p_ev) {
	switch (p_ev.type) {
		case SDL_QUIT:
			if (window_event_callback.is_valid()) {
				Variant ev = (int)WINDOW_EVENT_CLOSE_REQUEST;
				const Variant *a[1] = { &ev };
				Variant ret;
				Callable::CallError err;
				window_event_callback.callp(a, 1, ret, err);
			}
			break;
		case SDL_WINDOWEVENT:
			// Compare any SDL2 resize-event payload against our
			// authoritative panel size (DRM mode or GODOT_SDL2_PANEL_*
			// env override, whichever was used at init). Mismatches are
			// phantom events SDL2 emits while it does internal video-
			// backend bookkeeping (commonly 1024x768 during EGL setup
			// or texture preloading); forwarding them to godot makes
			// game-side Window.Size flip mid-boot and pushes the UI
			// into a sub-rect of the panel.
			//
			// Matches DO get forwarded — that keeps the door open for
			// any future real resize (e.g., DRM hotplug, modeset). The
			// branch is symmetric on every device because `window_size`
			// is set from authoritative sources at init.
			if (p_ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
					p_ev.window.event == SDL_WINDOWEVENT_RESIZED) {
				Size2i sdl_payload(p_ev.window.data1, p_ev.window.data2);
				if (use_sdl_gl) {
					// SDL-delegated (wayland/合成器): 合成器才是 surface 尺寸的权威。
					// 真实尺寸常在 init 之后随首个 configure 到达(init 只看到 1×1 占位)。
					// 接受任何 >1×1 的新尺寸,更新 window_size 并通知 godot 重设视口。
					// 不能套用下面的"幻影过滤"——那会把唯一一次真 resize 也丢掉(→ 偏移/只剩一角)。
					if (sdl_payload.width > 1 && sdl_payload.height > 1 && sdl_payload != window_size) {
						window_size = sdl_payload;
						print_verbose(vformat("DSDL2: SDL-GL accepted resize %dx%d", sdl_payload.width, sdl_payload.height));
						if (rect_changed_callback.is_valid()) {
							Variant r = Rect2i(window_position, window_size);
							const Variant *a[1] = { &r };
							Variant ret;
							Callable::CallError err;
							rect_changed_callback.callp(a, 1, ret, err);
						}
					}
				} else if (sdl_payload != window_size) {
					// dummy/KMS 路(TrimUI): SDL 在 EGL 初始化时发的假 resize —
					// 真尺寸以 DRM 面板为准,丢弃。Verbose 记一笔便于排查。
					print_verbose(vformat("DSDL2: dropped phantom SDL2 SIZE_CHANGED %dx%d (real %dx%d)",
							sdl_payload.width, sdl_payload.height,
							window_size.width, window_size.height));
				} else {
					if (rect_changed_callback.is_valid()) {
						Variant r = Rect2i(window_position, window_size);
						const Variant *a[1] = { &r };
						Variant ret;
						Callable::CallError err;
						rect_changed_callback.callp(a, 1, ret, err);
					}
				}
			}
			break;

		// Keyboard — translate SDL2 keycode to godot Key enum. Without this,
		// gptokeyb's synthesized keyboard events fall through to default:
		// break below and never reach godot's Input singleton, so godot UI
		// navigation (ui_accept etc.) never fires on handheld CFWs.
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			// SDL-delegated GL 模式下 SDL 也会从合成器拿到键盘事件,而我们仍直读
			// evdev(gptokeyb 的 uinput)→ 二者读同一来源会双份。evdev 作唯一来源,
			// 这里丢掉 SDL 的键事件。(dummy 模式 SDL 不探键盘,本就走不到这。)
			if (use_sdl_gl) {
				break;
			}
			Ref<InputEventKey> k;
			k.instantiate();
			k->set_pressed(p_ev.type == SDL_KEYDOWN);
			k->set_echo(p_ev.key.repeat != 0);
			Key mapped = _sdl_keycode_to_godot(p_ev.key.keysym.sym);
			k->set_keycode(mapped);
			k->set_physical_keycode(mapped);
			// Log the inbound key so a handheld test session can confirm
			// from godot.log that the event made it this far. If `mapped`
			// is NONE the _sdl_keycode_to_godot default branch already
			// logged the SDL name; here we log the mapped godot Key for
			// the success path too.
			print_verbose(vformat(
					"DisplayServerSDL2: SDL_KEY%s sdlk=0x%x → godot Key=0x%x repeat=%d",
					p_ev.type == SDL_KEYDOWN ? "DOWN" : "UP",
					(int)p_ev.key.keysym.sym, (int)mapped, (int)p_ev.key.repeat));
			Input::get_singleton()->parse_input_event(k);
		} break;

		// Gamepad button — for the launcher this is path B (no gptokeyb in
		// the mix). godot's Input::joy_button hands off to InputDefault which
		// dispatches InputEventJoypadButton for us.
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP: {
			// evdev 作唯一手柄来源(见上 KEY 注释),SDL-delegated GL 模式丢 SDL 手柄事件。
			if (use_sdl_gl) {
				break;
			}
			JoyButton btn = _sdl_controller_button_to_godot(
					(SDL_GameControllerButton)p_ev.cbutton.button);
			print_verbose(vformat(
					"DisplayServerSDL2: SDL_CONTROLLERBUTTON%s which=%d sdl_btn=%d → godot JoyButton=%d",
					p_ev.type == SDL_CONTROLLERBUTTONDOWN ? "DOWN" : "UP",
					p_ev.cbutton.which, (int)p_ev.cbutton.button, (int)btn));
			if (btn != JoyButton::INVALID) {
				Input::get_singleton()->joy_button(p_ev.cbutton.which, btn,
						p_ev.type == SDL_CONTROLLERBUTTONDOWN);
			}
		} break;

		// Gamepad axis (analog sticks + triggers).
		case SDL_CONTROLLERAXISMOTION: {
			if (use_sdl_gl) {
				break;
			}
			JoyAxis axis = _sdl_controller_axis_to_godot(
					(SDL_GameControllerAxis)p_ev.caxis.axis);
			if (axis != JoyAxis::INVALID) {
				// SDL axis is int16 [-32768, 32767]; godot wants -1..1.
				float v = p_ev.caxis.value / (p_ev.caxis.value < 0 ? 32768.0f : 32767.0f);
				// Verbose-only — axis motion fires every frame and would
				// swamp the log if always-on. Filter to > 0.3 deflection
				// so we still catch real input but skip resting noise.
				if (Math::abs(v) > 0.3f) {
					print_verbose(vformat(
							"DisplayServerSDL2: SDL_CONTROLLERAXISMOTION which=%d axis=%d → godot JoyAxis=%d v=%f",
							p_ev.caxis.which, (int)p_ev.caxis.axis, (int)axis, v));
				}
				Input::get_singleton()->joy_axis(p_ev.caxis.which, axis, v);
			}
		} break;

		// TODO future work: SDL_MOUSEMOTION / SDL_MOUSEBUTTON*, SDL_TEXTINPUT.
		default:
			break;
	}
}

void DisplayServerSDL2::process_events() {
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		_process_sdl_event(ev);
	}
	_process_evdev();
	Input::get_singleton()->flush_buffered_events();
}

// ===================================================================
// evdev fast path — bypass SDL2's input subsystem entirely
// ===================================================================
//
// Why we go around SDL: SDL_VIDEODRIVER=dummy disables the evdev
// keyboard/mouse probes (no window → no input). On TrimUI handhelds we
// can't run without dummy because SDL's KMSDRM driver collides with our
// own KMS+GBM+EGL bring-up (we own /dev/dri/card0, SDL would fight us
// for it). The kernel doesn't care which userspace process reads
// /dev/input/event*, so we just read the events ourselves.
//
// The evdev → godot translation we need is small:
//   * EV_KEY with code in KEY_* range → godot InputEventKey
//   * EV_KEY with code in BTN_GAMEPAD range → godot Input::joy_button
//   * EV_ABS on a joystick device → godot Input::joy_axis
//
// We classify devices once at startup by looking at their event bits via
// EVIOCGBIT. A device that exposes ABS_X and BTN_GAMEPAD is treated as a
// joystick; a device that exposes KEY_A is treated as a keyboard.
// (Some devices are both — e.g. composite gamepads with extra keys —
// and we set both flags; no harm done.)

// Map a kernel keycode (KEY_* from <linux/input-event-codes.h>) to godot
// Key. Only the keys gptokeyb produces by default + the common ASCII set;
// extend as needed. Unknown codes return Key::NONE and are dropped with
// a print_verbose so a deployed config flagging "key X doesn't work" has
// a breadcrumb in godot.log.
static Key _evdev_keycode_to_godot(uint16_t code) {
	switch (code) {
		// Letters (kernel uses ASCII letter codes, godot Key::A = 0x41).
		case KEY_A: return Key::A; case KEY_B: return Key::B; case KEY_C: return Key::C;
		case KEY_D: return Key::D; case KEY_E: return Key::E; case KEY_F: return Key::F;
		case KEY_G: return Key::G; case KEY_H: return Key::H; case KEY_I: return Key::I;
		case KEY_J: return Key::J; case KEY_K: return Key::K; case KEY_L: return Key::L;
		case KEY_M: return Key::M; case KEY_N: return Key::N; case KEY_O: return Key::O;
		case KEY_P: return Key::P; case KEY_Q: return Key::Q; case KEY_R: return Key::R;
		case KEY_S: return Key::S; case KEY_T: return Key::T; case KEY_U: return Key::U;
		case KEY_V: return Key::V; case KEY_W: return Key::W; case KEY_X: return Key::X;
		case KEY_Y: return Key::Y; case KEY_Z: return Key::Z;

		// Digits — kernel KEY_0..KEY_9 macros got undef'd at the top of
		// the file (they collide with godot Key::KEY_0..Key::KEY_9). Use
		// the cached LINUX_KEY_* constexpr ints instead.
		case LINUX_KEY_0: return Key::KEY_0; case LINUX_KEY_1: return Key::KEY_1;
		case LINUX_KEY_2: return Key::KEY_2; case LINUX_KEY_3: return Key::KEY_3;
		case LINUX_KEY_4: return Key::KEY_4; case LINUX_KEY_5: return Key::KEY_5;
		case LINUX_KEY_6: return Key::KEY_6; case LINUX_KEY_7: return Key::KEY_7;
		case LINUX_KEY_8: return Key::KEY_8; case LINUX_KEY_9: return Key::KEY_9;

		// Control / whitespace.
		case KEY_ENTER:     return Key::ENTER;       // gptokeyb A → Enter (ui_accept)
		case KEY_ESC:       return Key::ESCAPE;      // gptokeyb B → Esc   (ui_cancel)
		case KEY_BACKSPACE: return Key::BACKSPACE;
		case KEY_TAB:       return Key::TAB;
		case KEY_SPACE:     return Key::SPACE;

		// Arrow keys (gptokeyb's D-pad mapping).
		case KEY_UP:    return Key::UP;
		case KEY_DOWN:  return Key::DOWN;
		case KEY_LEFT:  return Key::LEFT;
		case KEY_RIGHT: return Key::RIGHT;

		// Modifiers.
		case KEY_LEFTSHIFT:
		case KEY_RIGHTSHIFT: return Key::SHIFT;
		case KEY_LEFTCTRL:
		case KEY_RIGHTCTRL:  return Key::CTRL;
		case KEY_LEFTALT:
		case KEY_RIGHTALT:   return Key::ALT;
		case KEY_LEFTMETA:
		case KEY_RIGHTMETA:  return Key::META;

		// Navigation.
		case KEY_HOME:     return Key::HOME;
		case KEY_END:      return Key::END;
		case KEY_PAGEUP:   return Key::PAGEUP;
		case KEY_PAGEDOWN: return Key::PAGEDOWN;
		case KEY_INSERT:   return Key::INSERT;
		// KEY_DELETE macro also undef'd at file top — use cached value.
		case LINUX_KEY_DELETE: return Key::KEY_DELETE;

		// Function keys F1-F12.
		case KEY_F1:  return Key::F1;  case KEY_F2:  return Key::F2;
		case KEY_F3:  return Key::F3;  case KEY_F4:  return Key::F4;
		case KEY_F5:  return Key::F5;  case KEY_F6:  return Key::F6;
		case KEY_F7:  return Key::F7;  case KEY_F8:  return Key::F8;
		case KEY_F9:  return Key::F9;  case KEY_F10: return Key::F10;
		case KEY_F11: return Key::F11; case KEY_F12: return Key::F12;

		default:
			print_verbose(vformat("DisplayServerSDL2/evdev: dropped EV_KEY code=%d (no godot mapping)", (int)code));
			return Key::NONE;
	}
}

// Map a kernel BTN_* gamepad code to godot JoyButton through the
// per-port InputRemap table. Anything not in the table returns INVALID
// and gets dropped — that's how a user disables a button by writing
// `a = NONE` in input_remap.cfg.
//
// The previous version was a hardcoded switch covering Xbox-360 layout;
// it now lives in InputRemap::populate_defaults() as the fallback when
// no cfg is present, so out-of-the-box behaviour is unchanged.
static JoyButton _evdev_button_to_godot(const InputRemap &remap, uint16_t code) {
	const JoyButton *p = remap.button_map.getptr(code);
	return p ? *p : JoyButton::INVALID;
}

// ABS_* axis code → godot JoyAxis. Sticks/triggers; hat (ABS_HAT0X/Y) is
// handled separately because it's an enum rather than a continuous axis.
static JoyAxis _evdev_axis_to_godot(uint16_t code) {
	switch (code) {
		case ABS_X:     return JoyAxis::LEFT_X;
		case ABS_Y:     return JoyAxis::LEFT_Y;
		case ABS_RX:    return JoyAxis::RIGHT_X;
		case ABS_RY:    return JoyAxis::RIGHT_Y;
		case ABS_Z:     return JoyAxis::TRIGGER_LEFT;
		case ABS_RZ:    return JoyAxis::TRIGGER_RIGHT;
		default:        return JoyAxis::INVALID;
	}
}

// Pull an EVIOCGBIT bitmask into a stack buffer and tell us whether a
// particular code is supported. We don't EVIOCGRAB because some real
// devices (TRIMUI Player1) are already grabbed by the system's MainUI;
// reading without grab is allowed and that's all we need.
static bool _evdev_has_bit(int fd, int evtype, int code) {
	uint8_t bits[(KEY_MAX / 8) + 1] = {};
	if (ioctl(fd, EVIOCGBIT(evtype, sizeof(bits)), bits) < 0) {
		return false;
	}
	return (bits[code / 8] & (1 << (code % 8))) != 0;
}

void DisplayServerSDL2::_scan_evdev() {
	// Load the per-port input_remap.cfg before opening any devices so
	// the first events we read can be looked up correctly. The cfg is
	// optional; load_or_default() falls back to baked-in defaults if
	// the file is absent.
	String loaded = input_remap.load_or_default();
	if (loaded.is_empty()) {
		print_verbose("DisplayServerSDL2/evdev: no input_remap.cfg found, using built-in defaults");
	} else {
		print_verbose(vformat("DisplayServerSDL2/evdev: input_remap.cfg loaded from %s", loaded));
	}

	DIR *dir = opendir("/dev/input");
	if (!dir) {
		ERR_PRINT(vformat("DisplayServerSDL2/evdev: opendir(/dev/input) failed: %s", strerror(errno)));
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != nullptr) {
		String name = String::utf8(de->d_name);
		if (!name.begins_with("event")) {
			continue;
		}
		String path = "/dev/input/" + name;
		int fd = open(path.utf8().get_data(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			print_verbose(vformat("DisplayServerSDL2/evdev: open(%s) failed: %s", path, strerror(errno)));
			continue;
		}

		EvdevHandle h;
		h.fd = fd;
		h.path = path;
		// Heuristic: anything that has KEY_A → keyboard; anything with BTN_GAMEPAD/SOUTH or ABS_X → joystick.
		h.is_keyboard = _evdev_has_bit(fd, EV_KEY, KEY_A);
		h.is_joystick = _evdev_has_bit(fd, EV_KEY, BTN_GAMEPAD) || _evdev_has_bit(fd, EV_KEY, BTN_SOUTH) || _evdev_has_bit(fd, EV_ABS, ABS_X);
		if (!h.is_keyboard && !h.is_joystick) {
			close(fd);
			continue;
		}
		if (h.is_joystick) {
			h.joy_id = Input::get_singleton()->get_unused_joy_id();
			char devname[256] = "evdev_joypad";
			ioctl(fd, EVIOCGNAME(sizeof(devname)), devname);
			Input::get_singleton()->joy_connection_changed(h.joy_id, true, String::utf8(devname), "");
			print_verbose(vformat("DisplayServerSDL2/evdev: joystick %s id=%d name=%s", path, (int)h.joy_id, String::utf8(devname)));
		}
		if (h.is_keyboard) {
			print_verbose(vformat("DisplayServerSDL2/evdev: keyboard %s", path));
		}
		evdev_handles.push_back(h);
	}
	closedir(dir);
	print_verbose(vformat("DisplayServerSDL2/evdev: scan complete, %d devices opened", evdev_handles.size()));
}

void DisplayServerSDL2::_close_evdev() {
	for (const EvdevHandle &h : evdev_handles) {
		if (h.fd >= 0) {
			close(h.fd);
		}
	}
	evdev_handles.clear();
}

void DisplayServerSDL2::_process_evdev() {
	struct input_event ev;
	for (const EvdevHandle &h : evdev_handles) {
		int events_this_device = 0;
		while (true) {
			ssize_t n = read(h.fd, &ev, sizeof(ev));
			if (n != (ssize_t)sizeof(ev)) {
				if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
					// Real error (not just "no data") — print once.
					static bool warned = false;
					if (!warned) {
						warned = true;
						print_verbose(vformat("DisplayServerSDL2/evdev: read(%s) error: %s",
								h.path, strerror(errno)));
					}
				}
				break; // EAGAIN, partial, or real error — done for this tick
			}
			events_this_device++;
			// Verbose log every event so we can see in godot.log exactly
			// what the kernel handed us. Without this it's impossible to
			// tell whether (a) we never get data (grab issue), (b) we
			// get data but the type/code doesn't map (translation gap),
			// or (c) we map fine but godot's Input subsystem swallows it.
			print_verbose(vformat(
					"DisplayServerSDL2/evdev: %s type=%d code=%d value=%d",
					h.path, (int)ev.type, (int)ev.code, (int)ev.value));
			switch (ev.type) {
				case EV_KEY: {
					// Gamepad button. Input::joy_button updates the
					// joypad state AND internally builds an
					// InputEventJoypadButton + calls parse_input_event,
					// so one call is enough — an earlier version of this
					// file ALSO ran parse_input_event explicitly to work
					// around an apparent delivery gap, but the real gap
					// was Input::set_event_dispatch_function missing from
					// the constructor; with that fixed the extra
					// parse_input_event becomes a duplicate that fires
					// every action twice (godot prints a "parsed more
					// than once" warning, and a single D-pad press skips
					// two list items).
					JoyButton jb = _evdev_button_to_godot(input_remap, ev.code);
					if (jb != JoyButton::INVALID && h.is_joystick) {
						Input::get_singleton()->joy_button(h.joy_id, jb, ev.value != 0);
						print_verbose(vformat(
								"DisplayServerSDL2/evdev: joy_button device=%d btn=%d pressed=%d",
								h.joy_id, (int)jb, (int)(ev.value != 0)));
						break;
					}
					// Keyboard key.
					Key k = _evdev_keycode_to_godot(ev.code);
					if (k != Key::NONE) {
						Ref<InputEventKey> ke;
						ke.instantiate();
						ke->set_pressed(ev.value != 0);
						ke->set_echo(ev.value == 2);
						ke->set_keycode(k);
						ke->set_physical_keycode(k);
						Input::get_singleton()->parse_input_event(ke);
						print_verbose(vformat(
								"DisplayServerSDL2/evdev: dispatched Key keycode=%d pressed=%d",
								(int)k, (int)(ev.value != 0)));
					}
				} break;
				case EV_ABS: {
					// DPad on TRIMUI / many handhelds is wired to
					// ABS_HAT0X / ABS_HAT0Y (digital pad reported as a
					// 3-valued axis -1/0/+1). godot expects DPad as
					// JoyButton::DPAD_*. Translate the HAT axis into two
					// independent press/release JoypadButton events so
					// godot UI ui_left/ui_right/ui_up/ui_down navigation
					// works out of the box.
					if (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) {
						if (!h.is_joystick) {
							break;
						}
						JoyButton neg = (ev.code == ABS_HAT0X) ? JoyButton::DPAD_LEFT : JoyButton::DPAD_UP;
						JoyButton pos = (ev.code == ABS_HAT0X) ? JoyButton::DPAD_RIGHT : JoyButton::DPAD_DOWN;
						// Release whichever side wasn't pressed this tick
						// + press the one that is. For value=0 (centered),
						// both get released. Input::joy_button does the
						// InputEvent creation + parse_input_event internally
						// (same reason we don't double-dispatch in the
						// EV_KEY branch above — would cause D-pad to skip
						// two items per press).
						Input::get_singleton()->joy_button(h.joy_id, neg, ev.value < 0);
						Input::get_singleton()->joy_button(h.joy_id, pos, ev.value > 0);
						print_verbose(vformat(
								"DisplayServerSDL2/evdev: HAT %s value=%d → neg=%d pos=%d",
								(ev.code == ABS_HAT0X) ? "X" : "Y",
								ev.value, (int)(ev.value < 0), (int)(ev.value > 0)));
						break;
					}
					JoyAxis ax = _evdev_axis_to_godot(ev.code);
					if (ax != JoyAxis::INVALID && h.is_joystick) {
						// We don't yet pull axis ranges via EVIOCGABS, so
						// assume signed 16-bit values: divide by 32768.
						// This is correct for most gamepads; analog sticks
						// with a different range need EVIOCGABS at scan
						// time. Triggers also need separate scaling (they
						// use unsigned 0..255 on some kernels). TODO when
						// a real game needs the precision.
						float v = ev.value / 32768.0f;
						Input::get_singleton()->joy_axis(h.joy_id, ax, v);
					}
				} break;
				default:
					break;
			}
		}
	}
}

// ===================================================================
// the 92 stubs(全 return 默认值,够 godot 不 crash)
// ===================================================================
#include "display_server_sdl2_stubs.cpp"

#endif // SDL2_ENABLED
