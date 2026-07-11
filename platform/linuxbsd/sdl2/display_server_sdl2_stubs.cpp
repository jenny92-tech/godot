Point2i DisplayServerSDL2::screen_get_position(int p_screen ) const { return Point2i(); }
Size2i DisplayServerSDL2::screen_get_size(int p_screen ) const { return Size2i(); }
Rect2i DisplayServerSDL2::screen_get_usable_rect(int p_screen ) const { return Rect2i(); }
int DisplayServerSDL2::screen_get_dpi(int p_screen ) const { return 0; }
float DisplayServerSDL2::screen_get_refresh_rate(int p_screen ) const { return 0; }
void DisplayServerSDL2::window_set_transient(WindowID p_window, WindowID p_parent) { }
void DisplayServerSDL2::window_set_max_size(const Size2i p_size, WindowID p_window ) { }
Size2i DisplayServerSDL2::window_get_max_size(WindowID p_window ) const { return Size2i(); }
void DisplayServerSDL2::window_set_min_size(const Size2i p_size, WindowID p_window ) { }
Size2i DisplayServerSDL2::window_get_min_size(WindowID p_window ) const { return Size2i(); }
bool DisplayServerSDL2::window_is_maximize_allowed(WindowID p_window ) const { return false; }
void DisplayServerSDL2::window_request_attention(WindowID p_window ) { }
void DisplayServerSDL2::window_move_to_foreground(WindowID p_window ) { }
bool DisplayServerSDL2::window_is_focused(WindowID p_window ) const { return false; }
bool DisplayServerSDL2::window_can_draw(WindowID p_window ) const { return false; }
