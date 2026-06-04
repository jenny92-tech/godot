#!/usr/bin/env python3
"""
Extract all pure-virtual declarations from servers/display_server.h
and generate the override skeleton for DisplayServerSDL2.

Output: stdout — two lines per virtual:
  - header form (`virtual ... override;`)
  - stub form  (`Type DisplayServerSDL2::method(...) { return Type(); }`)
"""
import re, sys

src = open("../../../servers/display_server.h").read()

# 手写的(在 display_server_sdl2.h 类体里直接 override 的)— 从 .gen 里排除掉避免重复
MANUAL = {
    "has_feature", "get_name", "get_screen_count", "get_primary_screen",
    "get_window_list", "get_window_at_screen_position",
    "window_set_title", "window_set_size", "window_get_size", "window_get_size_with_decorations",
    "window_set_position", "window_get_position", "window_get_position_with_decorations",
    "window_set_mode", "window_get_mode", "window_set_vsync_mode", "window_get_vsync_mode",
    "window_attach_instance_id", "window_get_attached_instance_id",
    "window_set_rect_changed_callback", "window_set_window_event_callback", "window_set_input_event_callback",
    "window_set_input_text_callback", "window_set_drop_files_callback",
    "window_set_current_screen", "window_get_current_screen",
    "window_set_flag", "window_get_flag",
    "can_any_window_draw", "process_events", "swap_buffers",
    "release_rendering_thread", "make_rendering_thread", "show_window",
}

# Match `virtual <ret-type> <name>(<args>) [const] = 0;` allowing multi-line args.
pat = re.compile(
    r"virtual\s+([^;{]+?)\s*=\s*0\s*;",
    re.DOTALL,
)

count = 0
header_lines = []
impl_lines = []
for m in pat.finditer(src):
    decl = re.sub(r"\s+", " ", m.group(1)).strip()
    # decl looks like: `void window_set_title(...) [const]`
    is_const = decl.endswith(" const")
    if is_const:
        decl = decl[:-len(" const")].rstrip()
    # split return type vs name(args)
    paren = decl.index("(")
    head = decl[:paren].rstrip()
    args = decl[paren:]
    # head is like "void window_set_title" or "Size2i window_get_min_size"
    sp = head.rfind(" ")
    ret_type = head[:sp].strip()
    name = head[sp+1:].strip()
    if name.startswith("*"):
        ret_type += " *"
        name = name[1:]
    if name in MANUAL:
        continue   # 已在主头/cpp 手写,跳过
    header_lines.append(f"\tvirtual {ret_type} {name}{args}{' const' if is_const else ''} override;")
    # Default-return stub
    rt = ret_type.replace(" *", "*").strip()
    if rt in ("void",):
        body = ""
    elif rt in ("bool",):
        body = " return false;"
    elif rt in ("int", "float", "double", "int64_t", "uint32_t"):
        body = " return 0;"
    elif rt.endswith("*"):
        body = " return nullptr;"
    elif rt in ("String", "Color", "Variant"):
        body = f" return {rt}();"
    elif rt == "ObjectID":
        body = " return ObjectID();"
    else:
        body = f" return {rt}();"
    args_impl = re.sub(r"=\s*[^,)]+", "", args)  # strip default args in cpp
    impl_lines.append(f"{ret_type} DisplayServerSDL2::{name}{args_impl}{' const' if is_const else ''} {{{body} }}")
    count += 1

print(f"// Generated: {count} pure-virtual overrides", file=sys.stderr)
print("// ===== HEADER =====")
for l in header_lines:
    print(l)
print()
print("// ===== IMPL STUBS =====")
for l in impl_lines:
    print(l)
