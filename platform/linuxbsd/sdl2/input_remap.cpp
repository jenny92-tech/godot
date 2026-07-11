/**************************************************************************/
/*  input_remap.cpp                                                       */
/**************************************************************************/
#ifdef SDL2_ENABLED

#include "input_remap.h"

#include "core/io/file_access.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"

#include <linux/input.h>

// linux/input-event-codes.h #defines KEY_0..KEY_9 / KEY_DELETE as plain
// integer macros that textually collide with godot's Key:: enum members
// of the same names. We don't touch Key:: in this file, but a future
// editor might — keep the undef list narrow so a stray Key::KEY_0
// doesn't compile to Key::11.
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

// ── Name tables ───────────────────────────────────────────────────────
// Two layers of name → enum translation, kept side-by-side so the
// editing rules are obvious:
//   * LOGICAL_TO_BTN  — the "a" / "l1" / "select" strings users write
//                       on the LEFT side of `key = value` lines, paired
//                       with the linux/input-event-codes.h BTN_* code
//                       they correspond to.
//   * BUTTON_TO_JOY   — the "BUTTON_A" / "BUTTON_L1" / "DPAD_UP" strings
//                       users write on the RIGHT side, paired with the
//                       godot JoyButton enum we'll dispatch as.
// Both lists mirror BogoDroid UnityLoader's wsm.toml vocabulary so the
// learning curve carries over for users coming from that ecosystem.

struct LogicalToBtn {
	const char *name; // What the user writes as the key in [buttons]
	int code;         // linux/input-event-codes.h value (the kernel
	                  // .code field of struct input_event)
};

// ── LOGICAL_TO_BTN: hardware-side vocabulary ──────────────────────────
// This table translates the LEFT side of every `key = value` rule in
// the cfg (e.g. the "a" in `a = BUTTON_A`) into the kernel BTN_* code
// the user's controller actually emits when that physical button is
// pressed.
//
// The "a/b/x/y" naming is position-based, not engraving-based — same
// vocabulary SDL GameController uses, same that wsm.toml uses:
//
//                   ┌─────────┐
//                   │    y    │     ← top face slot
//                   │ x     b │     ← left/right face slots
//                   │    a    │     ← bottom face slot = "a"
//                   └─────────┘
//
// On a Nintendo-layout handheld the BOTTOM slot is engraved "B" — the
// rule key is still "a". The mapping targets the position your thumb
// lands on at rest, not the silkscreened letter. Same applies to the
// X/Y swap between layouts. A cross-platform controller library can
// only be sane if it agrees on ONE position vocabulary; "a = bottom
// face button" is the universal one.
//
// (This is the #1 question new users ask: "why doesn't pressing A
// work?" — the answer is always "which A — the silkscreen or the
// JoyButton enum?". The hardware side here only ever means
// silkscreen-agnostic position.)
static const LogicalToBtn LOGICAL_TO_BTN[] = {
	// Face buttons (position-based, not letter-on-the-shell).
	{ "a",      BTN_SOUTH  }, // bottom face slot — kernel BTN_SOUTH = 304
	{ "b",      BTN_EAST   }, // right face slot  — kernel BTN_EAST  = 305
	{ "x",      BTN_WEST   }, // left face slot   — kernel BTN_WEST  = 308
	{ "y",      BTN_NORTH  }, // top face slot    — kernel BTN_NORTH = 307

	// Shoulders and triggers. l2/r2 on handhelds without analog
	// triggers (TRIMUI etc.) are digital buttons, mapped through the
	// PADDLE1/2 slots downstream.
	{ "l1",     BTN_TL     }, // 310 — top shoulder, left
	{ "r1",     BTN_TR     }, // 311 — top shoulder, right
	{ "l2",     BTN_TL2    }, // 312 — bottom shoulder / trigger, left
	{ "r2",     BTN_TR2    }, // 313 — bottom shoulder / trigger, right

	// System buttons.
	{ "select", BTN_SELECT }, // 314 — Select / Back / Minus
	{ "start",  BTN_START  }, // 315 — Start / Options / Plus
	{ "guide",  BTN_MODE   }, // 316 — Guide / Home / Xbox button

	// Analog stick clicks (l3 / r3 follow the Sony / Switch naming).
	{ "l3",     BTN_THUMBL }, // 317 — left stick push-in
	{ "r3",     BTN_THUMBR }, // 318 — right stick push-in

	// Some devices report the D-pad as discrete BTN_DPAD_* codes
	// instead of HAT axes. Include them so those controllers work
	// without a separate config. HAT-style D-pad on TRIMUI etc. is
	// handled directly in _process_evdev — it doesn't go through this
	// map at all.
	{ "dpup",    BTN_DPAD_UP    },
	{ "dpdown",  BTN_DPAD_DOWN  },
	{ "dpleft",  BTN_DPAD_LEFT  },
	{ "dpright", BTN_DPAD_RIGHT },
};

struct ButtonToJoy {
	const char *name;     // What the user writes as the value
	JoyButton value;      // The godot JoyButton we dispatch
};

// ── BUTTON_TO_JOY: engine-side vocabulary ────────────────────────────
// This table translates the RIGHT side of every `key = value` rule
// (e.g. the "BUTTON_A" in `a = BUTTON_A`) into the godot JoyButton
// enum we'll dispatch as InputEventJoypadButton when the corresponding
// physical button gets pressed.
//
// The BUTTON_* prefix matches BogoDroid wsm.toml exactly. From here
// godot's default UI bindings take over:
//   BUTTON_A      → JoyButton::A     → ui_accept (confirm)
//   BUTTON_B      → JoyButton::B     → ui_cancel (back)
//   DPAD_UP/etc.  → JoyButton::DPAD_* → ui_up / ui_down / …
//
// "NONE" maps to JoyButton::INVALID and is the explicit way to disable
// a button — pressing the physical button still fires kernel events,
// but the cfg drops them before they reach Input::joy_button.
static const ButtonToJoy BUTTON_TO_JOY[] = {
	// Face buttons.
	{ "BUTTON_A",      JoyButton::A              },
	{ "BUTTON_B",      JoyButton::B              },
	{ "BUTTON_X",      JoyButton::X              },
	{ "BUTTON_Y",      JoyButton::Y              },

	// Shoulders. godot has no dedicated L2/R2 JoyButton (the SDL
	// model treats triggers as analog axes), so BUTTON_L2/R2 borrow
	// the PADDLE1/2 slots — godot still recognises those for input
	// actions, and we get a unique InputEventJoypadButton per
	// physical button.
	{ "BUTTON_L1",     JoyButton::LEFT_SHOULDER  },
	{ "BUTTON_R1",     JoyButton::RIGHT_SHOULDER },
	{ "BUTTON_L2",     JoyButton::PADDLE1        },
	{ "BUTTON_R2",     JoyButton::PADDLE2        },

	// System / context.
	{ "BUTTON_SELECT", JoyButton::BACK           },
	{ "BUTTON_START",  JoyButton::START          },
	{ "BUTTON_MODE",   JoyButton::GUIDE          },

	// Analog stick clicks (godot's names for the press-in buttons).
	{ "BUTTON_THUMBL", JoyButton::LEFT_STICK     },
	{ "BUTTON_THUMBR", JoyButton::RIGHT_STICK    },

	// D-pad (for devices reporting via BTN_DPAD_*).
	{ "DPAD_UP",       JoyButton::DPAD_UP        },
	{ "DPAD_DOWN",     JoyButton::DPAD_DOWN      },
	{ "DPAD_LEFT",     JoyButton::DPAD_LEFT      },
	{ "DPAD_RIGHT",    JoyButton::DPAD_RIGHT     },

	// Escape hatch — explicit "this button does nothing".
	{ "NONE",          JoyButton::INVALID        },
};

static int _lookup_logical(const String &p_name) {
	for (const auto &row : LOGICAL_TO_BTN) {
		if (p_name == row.name) {
			return row.code;
		}
	}
	return -1;
}

static JoyButton _lookup_button(const String &p_name) {
	for (const auto &row : BUTTON_TO_JOY) {
		if (p_name == row.name) {
			return row.value;
		}
	}
	return JoyButton::INVALID;
}

// ── Defaults ──────────────────────────────────────────────────────────
// What ships when there's no input_remap.cfg next to godot.mono. Mirror
// of what the sample cfg writes; keep them in lock-step when one moves.
void InputRemap::populate_defaults() {
	button_map.clear();
	button_map[BTN_SOUTH]      = JoyButton::A;
	button_map[BTN_EAST]       = JoyButton::B;
	button_map[BTN_WEST]       = JoyButton::X;
	button_map[BTN_NORTH]      = JoyButton::Y;
	button_map[BTN_TL]         = JoyButton::LEFT_SHOULDER;
	button_map[BTN_TR]         = JoyButton::RIGHT_SHOULDER;
	button_map[BTN_TL2]        = JoyButton::PADDLE1;
	button_map[BTN_TR2]        = JoyButton::PADDLE2;
	button_map[BTN_SELECT]     = JoyButton::BACK;
	button_map[BTN_START]      = JoyButton::START;
	button_map[BTN_MODE]       = JoyButton::GUIDE;
	button_map[BTN_THUMBL]     = JoyButton::LEFT_STICK;
	button_map[BTN_THUMBR]     = JoyButton::RIGHT_STICK;
	button_map[BTN_DPAD_UP]    = JoyButton::DPAD_UP;
	button_map[BTN_DPAD_DOWN]  = JoyButton::DPAD_DOWN;
	button_map[BTN_DPAD_LEFT]  = JoyButton::DPAD_LEFT;
	button_map[BTN_DPAD_RIGHT] = JoyButton::DPAD_RIGHT;
}

// ── INI parser ─────────────────────────────────────────────────────────
// Tiny hand-rolled INI reader.
//   * Lines starting with '#' or ';' are comments and dropped.
//   * Empty lines dropped.
//   * `[section]` switches the current section. Only [buttons] is
//     honoured — anything else is logged as "unknown section, content
//     skipped" so a typo in the header doesn't silently wipe a section.
//   * `key = value` adds an entry to the current section.
//   * Inline comments after a value (`a = BUTTON_A  # face A`) are
//     stripped at parse time.
//   * Values can be quoted ("BUTTON_A") to match wsm.toml conventions;
//     we trim the quotes if present.
// Anything else logs a warning and is skipped — better to keep working
// defaults than to silently swallow a typo.

static String _strip_inline_comment(const String &p_in) {
	int hash = p_in.find("#");
	int semi = p_in.find(";");
	int cut = -1;
	if (hash >= 0 && (semi < 0 || hash < semi)) {
		cut = hash;
	} else if (semi >= 0) {
		cut = semi;
	}
	return (cut >= 0) ? p_in.substr(0, cut) : p_in;
}

static String _unquote(const String &p_in) {
	if (p_in.length() >= 2 && p_in.begins_with("\"") && p_in.ends_with("\"")) {
		return p_in.substr(1, p_in.length() - 2);
	}
	return p_in;
}

bool InputRemap::parse_file(const String &p_path) {
	if (!FileAccess::exists(p_path)) {
		return false;
	}
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		print_verbose(vformat("InputRemap: %s exists but open failed", p_path));
		return false;
	}

	String section;
	int loaded = 0;
	int line_no = 0;

	while (!f->eof_reached()) {
		String line = f->get_line();
		line_no++;
		line = _strip_inline_comment(line).strip_edges();
		if (line.is_empty()) {
			continue;
		}
		if (line.begins_with("[") && line.ends_with("]")) {
			section = line.substr(1, line.length() - 2).strip_edges();
			continue;
		}
		int eq = line.find("=");
		if (eq <= 0) {
			print_verbose(vformat("InputRemap: %s:%d not a section / KEY=VALUE — skipped: %s",
					p_path, line_no, line));
			continue;
		}
		String key = line.substr(0, eq).strip_edges().to_lower();
		String value = _unquote(line.substr(eq + 1, line.length()).strip_edges());

		if (section != "buttons") {
			print_verbose(vformat("InputRemap: %s:%d entry outside [buttons] section [%s] — skipped",
					p_path, line_no, section));
			continue;
		}

		int code = _lookup_logical(key);
		if (code < 0) {
			print_verbose(vformat("InputRemap: %s:%d unknown logical button '%s' — skipped",
					p_path, line_no, key));
			continue;
		}
		JoyButton jb = _lookup_button(value);
		if (jb == JoyButton::INVALID && value != "NONE") {
			print_verbose(vformat("InputRemap: %s:%d unknown button value '%s' — skipped",
					p_path, line_no, value));
			continue;
		}
		// NONE → drop any existing binding (so user can explicitly
		// disable a button by writing `a = NONE`).
		if (jb == JoyButton::INVALID) {
			button_map.erase(code);
		} else {
			button_map[code] = jb;
		}
		loaded++;
	}

	print_verbose(vformat("InputRemap: loaded %s — %d entries", p_path, loaded));
	return true;
}

String InputRemap::load_or_default() {
	populate_defaults();
	// Per-port file wins over a shared one at the parent level.
	static const char *paths[] = {
		"./input_remap.cfg",
		"../input_remap.cfg",
	};
	for (const char *p : paths) {
		if (parse_file(p)) {
			return p;
		}
	}
	return String();
}

#endif // SDL2_ENABLED
