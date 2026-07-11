/**************************************************************************/
/*  input_remap.h                                                         */
/**************************************************************************/
/* Per-port gamepad button remap loaded from input_remap.cfg next to     */
/* godot.mono. The format mirrors BogoDroid UnityLoader's wsm.toml       */
/* [input.remap] section so users coming from that ecosystem don't have  */
/* to learn anything new.                                                */
/*                                                                        */
/* ── How each remap rule is read ──────────────────────────────────────  */
/* A line in the cfg has two sides separated by `=`:                     */
/*                                                                        */
/*         a        =        BUTTON_A                                    */
/*         ^^^                ^^^^^^^^                                   */
/*         │                  └── ENGINE side — what godot thinks got    */
/*         │                       pressed. Drives default UI actions    */
/*         │                       (ui_accept / ui_cancel) and whatever  */
/*         │                       the game registers in its InputMap.   */
/*         └────────────────────── HARDWARE side — which physical        */
/*                                 button the user pressed on the        */
/*                                 controller.                           */
/*                                                                        */
/* This split matters because: (a) NEW USERS confuse the two and ask     */
/* "why doesn't pressing 'A' work?" — answer: the question is which A,   */
/* the silkscreened letter or the JoyButton enum. (b) Nintendo and Xbox  */
/* layouts engrave different letters on the same physical button slot,  */
/* so a "remap" only makes sense once we agree on a position-based       */
/* hardware vocabulary.                                                  */
/*                                                                        */
/* ── Hardware-side vocabulary (left of `=`) ───────────────────────────  */
/* LOGICAL positions, not engravings:                                    */
/*                                                                        */
/*                   ┌─────────┐                                         */
/*                   │    y    │  ← top face slot                        */
/*                   │ x     b │  ← left / right face slots              */
/*                   │    a    │  ← bottom face slot  (this is "a")      */
/*                   └─────────┘                                         */
/*                                                                        */
/* On Nintendo handhelds the BOTTOM slot is engraved "B" — it's still    */
/* "a" in this config. The remap targets position, not the printed       */
/* letter. Same applies to the X/Y swap between layouts.                 */
/*                                                                        */
/* Full vocabulary: a / b / x / y / l1 / r1 / l2 / r2 / select / start / */
/* guide / l3 / r3 / dpup / dpdown / dpleft / dpright.                  */
/*                                                                        */
/* ── Engine-side vocabulary (right of `=`) ────────────────────────────  */
/* godot JoyButton enums, in wsm.toml notation:                          */
/*   BUTTON_A / B / X / Y         → JoyButton::A / B / X / Y            */
/*   BUTTON_L1 / R1               → JoyButton::LEFT_SHOULDER / RIGHT_SHOULDER */
/*   BUTTON_L2 / R2               → JoyButton::PADDLE1 / PADDLE2        */
/*                                  (godot has no L2/R2 button — its   */
/*                                  triggers are analog axes — so       */
/*                                  digital L2/R2 borrow the PADDLE     */
/*                                  slots)                              */
/*   BUTTON_SELECT / START / MODE → JoyButton::BACK / START / GUIDE     */
/*   BUTTON_THUMBL / THUMBR       → JoyButton::LEFT_STICK / RIGHT_STICK */
/*   DPAD_UP / DOWN / LEFT / RIGHT → JoyButton::DPAD_*                  */
/*   NONE                         → JoyButton::INVALID (button disabled) */
/*                                                                        */
/* ── What's intentionally NOT configurable ────────────────────────────  */
/* * D-pad (ABS_HAT0X/Y): hardcoded to DPAD_LEFT/RIGHT/UP/DOWN — no one  */
/*   ever wants to remap "D-pad left" to right.                          */
/* * Analog sticks / triggers (ABS_X/Y/RX/RY/Z/RZ): mapped directly to   */
/*   godot's JoyAxis layout (LEFT_X/Y, RIGHT_X/Y, TRIGGER_LEFT/RIGHT) — */
/*   these are SDL convention and games depend on them.                  */
/*                                                                        */
/* Out of the box (no cfg present) populate_defaults() writes a 1:1      */
/* mapping (a→BUTTON_A, b→BUTTON_B, l1→BUTTON_L1, …) that matches every  */
/* mainstream handheld. The cfg only earns its keep when someone wants   */
/* to swap or disable specific buttons.                                  */
/**************************************************************************/
#pragma once

#ifdef SDL2_ENABLED

#include "core/input/input_enums.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"

class InputRemap {
public:
	// Final lookup table the event loop uses: kernel BTN_* code →
	// godot JoyButton. Populated by populate_defaults() and then merged
	// with whatever the .cfg overrides. Missing keys mean "no event for
	// this physical button" — same as the user writing NONE.
	HashMap<int, JoyButton> button_map;

	// Build with hardcoded defaults — used as the baseline before
	// parse_file() merges user overrides on top.
	void populate_defaults();

	// Parse a config file at `p_path`. Returns true if the file was
	// found AND opened; false if it doesn't exist (caller falls back
	// to defaults silently). Unknown logical names (a typo'd key or
	// value, or a name not in the wsm.toml vocabulary) log a warning
	// and skip the line — the goal is never to let one bad line
	// shadow a working default.
	bool parse_file(const String &p_path);

	// One-shot helper used at DisplayServerSDL2 startup. Calls
	// populate_defaults() first, then tries each path in order until
	// one loads. Returns the path actually loaded, or "" if only
	// defaults are active.
	String load_or_default();
};

#endif // SDL2_ENABLED
