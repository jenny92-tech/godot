/**************************************************************************/
/*  display_server_sdl2.cpp                                               */
/**************************************************************************/
#ifdef SDL2_ENABLED

#include "display_server_sdl2.h"

#include "core/config/project_settings.h"
#include "main/main.h"

// Full SDL2 header(SDL_Init / SDL_CreateWindow / SDL_GL_* / SDL_Event 等),只在 .cpp 包。
// .h 只前向声明 SDL_Window/SDL_GLContext/SDL_Event,避免与 godot drivers/sdl/joypad_sdl.h
// 的 SDL_JoystickID typedef 冲突。
#include <SDL2/SDL.h>

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
	window_size = p_resolution;
	if (window_size == Size2i()) {
		window_size = Size2i(1280, 720);
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
		ERR_PRINT(vformat("SDL_Init failed: %s", SDL_GetError()));
		r_error = ERR_UNAVAILABLE;
		return;
	}

	// GL ES 3.0 context — Mali handhelds rarely have full GL 3, but always have GLES 3.x.
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

	uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
	if (p_mode == WINDOW_MODE_FULLSCREEN || p_mode == WINDOW_MODE_EXCLUSIVE_FULLSCREEN) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}

	window = SDL_CreateWindow("Godot", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			window_size.width, window_size.height, flags);
	if (!window) {
		ERR_PRINT(vformat("SDL_CreateWindow failed: %s", SDL_GetError()));
		SDL_Quit();
		r_error = ERR_UNAVAILABLE;
		return;
	}

	gl_context = SDL_GL_CreateContext(window);
	if (!gl_context) {
		ERR_PRINT(vformat("SDL_GL_CreateContext failed: %s", SDL_GetError()));
		SDL_DestroyWindow(window);
		SDL_Quit();
		r_error = ERR_UNAVAILABLE;
		return;
	}

	SDL_GL_SetSwapInterval(vsync_mode == VSYNC_ENABLED ? 1 : 0);

	int w, h;
	SDL_GL_GetDrawableSize(window, &w, &h);
	window_size = Size2i(w, h);

	window_mode = p_mode;
	window_visible = true;

	print_verbose(vformat("DisplayServerSDL2: window %dx%d, GL context OK", w, h));
}

DisplayServerSDL2::~DisplayServerSDL2() {
	if (gl_context) {
		SDL_GL_DeleteContext(gl_context);
		gl_context = nullptr;
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
	if (window) {
		SDL_SetWindowSize(window, p_size.width, p_size.height);
		window_size = p_size;
	}
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
	SDL_GL_SetSwapInterval(p_vsync_mode == VSYNC_ENABLED ? 1 : 0);
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
	if (window) {
		SDL_GL_SwapWindow(window);
	}
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
			if (p_ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
					p_ev.window.event == SDL_WINDOWEVENT_RESIZED) {
				window_size = Size2i(p_ev.window.data1, p_ev.window.data2);
				if (rect_changed_callback.is_valid()) {
					Variant r = Rect2i(window_position, window_size);
					const Variant *a[1] = { &r };
					Variant ret;
					Callable::CallError err;
					rect_changed_callback.callp(a, 1, ret, err);
				}
			}
			break;
		// TODO: SDL_KEYDOWN/UP, SDL_MOUSEMOTION, SDL_MOUSEBUTTON{DOWN,UP} → godot InputEventKey/MouseMotion/MouseButton
		// Skipped for POC — godot main loop will run + render even with no input.
		default:
			break;
	}
}

void DisplayServerSDL2::process_events() {
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		_process_sdl_event(ev);
	}
	Input::get_singleton()->flush_buffered_events();
}

// ===================================================================
// the 92 stubs(全 return 默认值,够 godot 不 crash)
// ===================================================================
#include "display_server_sdl2_stubs.cpp"

#endif // SDL2_ENABLED
