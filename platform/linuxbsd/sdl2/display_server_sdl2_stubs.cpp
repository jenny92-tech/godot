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
Error DisplayServerSDL2::init() { return Error(); }
bool DisplayServerSDL2::window_create(DisplayServer::WindowID p_window_id, void *p_handle) { return false; }
void DisplayServerSDL2::window_destroy(DisplayServer::WindowID p_window_id) { }
RID DisplayServerSDL2::accessibility_create_element(DisplayServer::WindowID p_window_id, DisplayServer::AccessibilityRole p_role) { return RID(); }
RID DisplayServerSDL2::accessibility_create_sub_element(const RID &p_parent_rid, DisplayServer::AccessibilityRole p_role, int p_insert_pos ) { return RID(); }
RID DisplayServerSDL2::accessibility_create_sub_text_edit_elements(const RID &p_parent_rid, const RID &p_shaped_text, float p_min_height, int p_insert_pos ) { return RID(); }
bool DisplayServerSDL2::accessibility_has_element(const RID &p_id) const { return false; }
void DisplayServerSDL2::accessibility_free_element(const RID &p_id) { }
void DisplayServerSDL2::accessibility_element_set_meta(const RID &p_id, const Variant &p_meta) { }
Variant DisplayServerSDL2::accessibility_element_get_meta(const RID &p_id) const { return Variant(); }
void DisplayServerSDL2::accessibility_update_if_active(const Callable &p_callable) { }
RID DisplayServerSDL2::accessibility_get_window_root(DisplayServer::WindowID p_window_id) const { return RID(); }
void DisplayServerSDL2::accessibility_update_set_focus(const RID &p_id) { }
void DisplayServerSDL2::accessibility_set_window_rect(DisplayServer::WindowID p_window_id, const Rect2 &p_rect_out, const Rect2 &p_rect_in) { }
void DisplayServerSDL2::accessibility_set_window_focused(DisplayServer::WindowID p_window_id, bool p_focused) { }
void DisplayServerSDL2::accessibility_update_set_role(const RID &p_id, DisplayServer::AccessibilityRole p_role) { }
void DisplayServerSDL2::accessibility_update_set_name(const RID &p_id, const String &p_name) { }
void DisplayServerSDL2::accessibility_update_set_extra_info(const RID &p_id, const String &p_name_extra_info) { }
void DisplayServerSDL2::accessibility_update_set_description(const RID &p_id, const String &p_description) { }
void DisplayServerSDL2::accessibility_update_set_value(const RID &p_id, const String &p_value) { }
void DisplayServerSDL2::accessibility_update_set_tooltip(const RID &p_id, const String &p_tooltip) { }
void DisplayServerSDL2::accessibility_update_set_bounds(const RID &p_id, const Rect2 &p_rect) { }
void DisplayServerSDL2::accessibility_update_set_transform(const RID &p_id, const Transform2D &p_transform) { }
void DisplayServerSDL2::accessibility_update_add_child(const RID &p_id, const RID &p_child_id) { }
void DisplayServerSDL2::accessibility_update_add_related_controls(const RID &p_id, const RID &p_related_id) { }
void DisplayServerSDL2::accessibility_update_add_related_details(const RID &p_id, const RID &p_related_id) { }
void DisplayServerSDL2::accessibility_update_add_related_described_by(const RID &p_id, const RID &p_related_id) { }
void DisplayServerSDL2::accessibility_update_add_related_flow_to(const RID &p_id, const RID &p_related_id) { }
void DisplayServerSDL2::accessibility_update_add_related_labeled_by(const RID &p_id, const RID &p_related_id) { }
void DisplayServerSDL2::accessibility_update_add_related_radio_group(const RID &p_id, const RID &p_related_id) { }
void DisplayServerSDL2::accessibility_update_set_active_descendant(const RID &p_id, const RID &p_other_id) { }
void DisplayServerSDL2::accessibility_update_set_next_on_line(const RID &p_id, const RID &p_other_id) { }
void DisplayServerSDL2::accessibility_update_set_previous_on_line(const RID &p_id, const RID &p_other_id) { }
void DisplayServerSDL2::accessibility_update_set_member_of(const RID &p_id, const RID &p_group_id) { }
void DisplayServerSDL2::accessibility_update_set_in_page_link_target(const RID &p_id, const RID &p_other_id) { }
void DisplayServerSDL2::accessibility_update_set_error_message(const RID &p_id, const RID &p_other_id) { }
void DisplayServerSDL2::accessibility_update_set_live(const RID &p_id, DisplayServer::AccessibilityLiveMode p_live) { }
void DisplayServerSDL2::accessibility_update_add_action(const RID &p_id, DisplayServer::AccessibilityAction p_action, const Callable &p_callable) { }
void DisplayServerSDL2::accessibility_update_add_custom_action(const RID &p_id, int p_action_id, const String &p_action_description) { }
void DisplayServerSDL2::accessibility_update_set_table_row_count(const RID &p_id, int p_count) { }
void DisplayServerSDL2::accessibility_update_set_table_column_count(const RID &p_id, int p_count) { }
void DisplayServerSDL2::accessibility_update_set_table_row_index(const RID &p_id, int p_index) { }
void DisplayServerSDL2::accessibility_update_set_table_column_index(const RID &p_id, int p_index) { }
void DisplayServerSDL2::accessibility_update_set_table_cell_position(const RID &p_id, int p_row_index, int p_column_index) { }
void DisplayServerSDL2::accessibility_update_set_table_cell_span(const RID &p_id, int p_row_span, int p_column_span) { }
void DisplayServerSDL2::accessibility_update_set_list_item_count(const RID &p_id, int p_size) { }
void DisplayServerSDL2::accessibility_update_set_list_item_index(const RID &p_id, int p_index) { }
void DisplayServerSDL2::accessibility_update_set_list_item_level(const RID &p_id, int p_level) { }
void DisplayServerSDL2::accessibility_update_set_list_item_selected(const RID &p_id, bool p_selected) { }
void DisplayServerSDL2::accessibility_update_set_list_item_expanded(const RID &p_id, bool p_expanded) { }
void DisplayServerSDL2::accessibility_update_set_popup_type(const RID &p_id, DisplayServer::AccessibilityPopupType p_popup) { }
void DisplayServerSDL2::accessibility_update_set_checked(const RID &p_id, bool p_checekd) { }
void DisplayServerSDL2::accessibility_update_set_num_value(const RID &p_id, double p_position) { }
void DisplayServerSDL2::accessibility_update_set_num_range(const RID &p_id, double p_min, double p_max) { }
void DisplayServerSDL2::accessibility_update_set_num_step(const RID &p_id, double p_step) { }
void DisplayServerSDL2::accessibility_update_set_num_jump(const RID &p_id, double p_jump) { }
void DisplayServerSDL2::accessibility_update_set_scroll_x(const RID &p_id, double p_position) { }
void DisplayServerSDL2::accessibility_update_set_scroll_x_range(const RID &p_id, double p_min, double p_max) { }
void DisplayServerSDL2::accessibility_update_set_scroll_y(const RID &p_id, double p_position) { }
void DisplayServerSDL2::accessibility_update_set_scroll_y_range(const RID &p_id, double p_min, double p_max) { }
void DisplayServerSDL2::accessibility_update_set_text_decorations(const RID &p_id, bool p_underline, bool p_strikethrough, bool p_overline) { }
void DisplayServerSDL2::accessibility_update_set_text_align(const RID &p_id, HorizontalAlignment p_align) { }
void DisplayServerSDL2::accessibility_update_set_text_selection(const RID &p_id, const RID &p_text_start_id, int p_start_char, const RID &p_text_end_id, int p_end_char) { }
void DisplayServerSDL2::accessibility_update_set_flag(const RID &p_id, DisplayServer::AccessibilityFlags p_flag, bool p_value) { }
void DisplayServerSDL2::accessibility_update_set_classname(const RID &p_id, const String &p_classname) { }
void DisplayServerSDL2::accessibility_update_set_placeholder(const RID &p_id, const String &p_placeholder) { }
void DisplayServerSDL2::accessibility_update_set_language(const RID &p_id, const String &p_language) { }
void DisplayServerSDL2::accessibility_update_set_text_orientation(const RID &p_id, bool p_vertical) { }
void DisplayServerSDL2::accessibility_update_set_list_orientation(const RID &p_id, bool p_vertical) { }
void DisplayServerSDL2::accessibility_update_set_shortcut(const RID &p_id, const String &p_shortcut) { }
void DisplayServerSDL2::accessibility_update_set_url(const RID &p_id, const String &p_url) { }
void DisplayServerSDL2::accessibility_update_set_role_description(const RID &p_id, const String &p_description) { }
void DisplayServerSDL2::accessibility_update_set_state_description(const RID &p_id, const String &p_description) { }
void DisplayServerSDL2::accessibility_update_set_color_value(const RID &p_id, const Color &p_color) { }
void DisplayServerSDL2::accessibility_update_set_background_color(const RID &p_id, const Color &p_color) { }
void DisplayServerSDL2::accessibility_update_set_foreground_color(const RID &p_id, const Color &p_color) { }
