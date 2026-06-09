/**************************************************************************/
/*  display_server_sdl2.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* SDL2-backed DisplayServer for embedded Linux handhelds (PortMaster /   */
/* arm64 Mali/Adreno SBCs). SDL2 handles EGL display/surface/context via  */
/* its KMSDRM video driver (env SDL_VIDEODRIVER=kmsdrm) — bypassing       */
/* EGL_KHR_platform_x11/wayland, which Mali libmali variants drop on these*/
/* devices (only EGL_KHR_platform_gbm is exposed). godot's GLES3 rasterizer*/
/* just needs a current GL context and eglGetProcAddress; SDL2 supplies   */
/* both. See platform/linuxbsd/sdl2/README.md for the architecture story. */
/**************************************************************************/
#pragma once

#ifdef SDL2_ENABLED

#include "core/input/input.h"
#include "input_remap.h"
#include "servers/display_server.h"

// 头文件只用前向声明,实质 include 全在 .cpp。
struct SDL_Window;
union SDL_Event;
class KMSGBMDevice;
class KMSEGLContext;

class DisplayServerSDL2 : public DisplayServer {
	GDSOFTCLASS(DisplayServerSDL2, DisplayServer);

	// SDL2 仅用于 events / joystick 输入(VIDEODRIVER=dummy,SDL2 不碰显示)。
	SDL_Window *window = nullptr;

	// 我们自己的 KMS+GBM+EGL 三件套:
	// - KMSGBMDevice  自己开 /dev/dri/card0 + gbm 设备/表面
	// - KMSEGLContext 自己最小化 EGL init,跳过 godot EGLManager 的 probe 模式
	//   (那套模式给闭源 Mali 多 2 倍接触面,我们这里走 FRT 4 直 libEGL 套路)
	KMSGBMDevice *kms_dev = nullptr;
	KMSEGLContext *egl_ctx = nullptr;

	String rendering_driver;
	Size2i window_size;
	Point2i window_position;
	// Panel rotation in degrees (0/90/180/270). Cached from
	// GODOT_SDL2_ROTATION env at DSDL2 init. Currently informational
	// only — actual content rotation is per-port (game-level swap-
	// rotate, godot Viewport transform, or DRM rotation property)
	// and not applied here. Will be wired up when MiniLoong-style
	// portrait-panel ports come online.
	int panel_rotation = 0;
	WindowMode window_mode = WINDOW_MODE_WINDOWED;
	VSyncMode vsync_mode = VSYNC_ENABLED;
	uint32_t window_flags = 0;
	bool window_visible = true;

	ObjectID window_attached_instance_id;
	Callable rect_changed_callback;
	Callable window_event_callback;
	Callable input_event_callback;
	Callable input_text_callback;
	Callable drop_files_callback;

	// Event dispatch helpers
	void _dispatch_event(const Ref<InputEvent> &p_event);
	void _process_sdl_event(const SDL_Event &p_ev);

	// Static wrapper registered with Input::set_event_dispatch_function in
	// the constructor. Every other DisplayServer (X11/Wayland/Windows/
	// Android/Web) registers a function like this so events that go
	// through Input::parse_input_event eventually reach the SceneTree's
	// input dispatch path (and thus script _input(event) callbacks).
	// Without this, parse_input_event updates joy/key state internally
	// but the event never fires _input — which is exactly the bug that
	// kept gamepad input from working on this fork before.
	static void _dispatch_input_events(const Ref<InputEvent> &p_event);

	// ── evdev input fast path ────────────────────────────────────────
	// We hack the video stack (KMS+GBM+EGL bypassing SDL's video driver
	// because Mali libmali rejects EGL_KHR_platform_x11) — by symmetry we
	// hack the input stack too: SDL2's "dummy" video driver disables
	// keyboard/mouse evdev probing, and on TrimUI-style handhelds the
	// system MainUI / keymon / OSD daemons EVIOCGRAB the real /dev/input
	// devices anyway. We bypass SDL by opening /dev/input/event* directly
	// and reading struct input_event ourselves. See _scan_evdev() in the
	// .cpp for the device-classification heuristic.
	struct EvdevHandle {
		int fd = -1;
		String path;
		bool is_keyboard = false;
		bool is_joystick = false;
		uint32_t joy_id = 0; // godot Input joy device id; only valid if is_joystick
	};
	Vector<EvdevHandle> evdev_handles;

	// Per-port button remap loaded from ./input_remap.cfg (or built-in
	// defaults if the cfg is absent). The button_map is keyed by kernel
	// BTN_* code, value is the JoyButton the event loop dispatches. HAT
	// (D-pad) is intentionally not configurable — see _process_evdev for
	// the hardcoded HAT0X/Y → DPAD_* mapping.
	InputRemap input_remap;

	void _scan_evdev();
	void _close_evdev();
	void _process_evdev();

	static DisplayServer *create_func(const String &p_rendering_driver, WindowMode p_mode, VSyncMode p_vsync_mode,
			uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution,
			int p_screen, Context p_context, int64_t p_parent_window, Error &r_error);
	static Vector<String> get_rendering_drivers_func();

public:
	static void register_sdl2_driver();

	DisplayServerSDL2(const String &p_rendering_driver, WindowMode p_mode, VSyncMode p_vsync_mode,
			uint32_t p_flags, const Vector2i &p_resolution, Context p_context, Error &r_error);
	~DisplayServerSDL2();

	// ===== Methods we actually implement(不走 stub)=====
	virtual bool has_feature(Feature p_feature) const override;
	virtual String get_name() const override;
	virtual int get_screen_count() const override;
	virtual int get_primary_screen() const override;
	virtual Vector<WindowID> get_window_list() const override;
	virtual WindowID get_window_at_screen_position(const Point2i &p_position) const override;
	virtual void window_set_title(const String &p_title, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual void window_set_size(const Size2i p_size, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual Size2i window_get_size(WindowID p_window = MAIN_WINDOW_ID) const override;
	virtual Size2i window_get_size_with_decorations(WindowID p_window = MAIN_WINDOW_ID) const override;
	virtual void window_set_position(const Point2i &p_position, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual Point2i window_get_position(WindowID p_window = MAIN_WINDOW_ID) const override;
	virtual Point2i window_get_position_with_decorations(WindowID p_window = MAIN_WINDOW_ID) const override;
	virtual void window_set_mode(WindowMode p_mode, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual WindowMode window_get_mode(WindowID p_window = MAIN_WINDOW_ID) const override;
	virtual void window_set_vsync_mode(VSyncMode p_vsync_mode, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual VSyncMode window_get_vsync_mode(WindowID p_window) const override;
	virtual void window_attach_instance_id(ObjectID p_instance, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual ObjectID window_get_attached_instance_id(WindowID p_window = MAIN_WINDOW_ID) const override;
	virtual void window_set_rect_changed_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual void window_set_window_event_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual void window_set_input_event_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual void window_set_input_text_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual void window_set_drop_files_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual void window_set_current_screen(int p_screen, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual int window_get_current_screen(WindowID p_window = MAIN_WINDOW_ID) const override;
	virtual void window_set_flag(WindowFlags p_flag, bool p_enabled, WindowID p_window = MAIN_WINDOW_ID) override;
	virtual bool window_get_flag(WindowFlags p_flag, WindowID p_window = MAIN_WINDOW_ID) const override;
	virtual bool can_any_window_draw() const override;
	virtual void process_events() override;
	virtual void swap_buffers() override;
	virtual void show_window(WindowID p_window) override;
	// release_rendering_thread / make_rendering_thread:non-pure 虚函数,不强制 override;
	// 我们也不需要管(godot 4 已经废弃多线程渲染上下文切换需求),让基类默认空实现走。

	// ===== 其他 ~85 stub 方法,从 servers/display_server.h 抽出 =====
	#include "display_server_sdl2_stubs.h"
};

#endif // SDL2_ENABLED
