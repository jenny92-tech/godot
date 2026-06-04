/**************************************************************************/
/*  kms_gbm_device.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Self-contained Linux KMS/DRM + GBM device wrapper. Used by the SDL2    */
/* DisplayServer's KMSDRM path to bypass SDL2's built-in KMSDRM video     */
/* driver, which has NULL-deref bugs on closed Mali libmali drivers      */
/* (TrimUI Smart Pro, MiniLoong Pocket One, etc.). We do the standard    */
/* Linux DRM page-flip sequence ourselves — every call is in our hands, */
/* nothing happens in SDL2 internals.                                     */
/*                                                                        */
/* References: drm-tut (kmscube), FRT 4.1.3 binary's NEEDED set (libdrm  */
/* + libgbm + libEGL + libGLESv2 + libSDL2 only for window/events).      */
/**************************************************************************/

#pragma once

#ifdef SDL2_ENABLED

#include "core/string/ustring.h"
#include "core/typedefs.h"

#include <stdint.h>

// Forward-declare libdrm/libgbm structs so the header doesn't need their headers.
struct gbm_device;
struct gbm_surface;
struct gbm_bo;
struct _drmModeRes;
struct _drmModeConnector;
struct _drmModeEncoder;
struct _drmModeCrtc;
struct _drmModeModeInfo;

class KMSGBMDevice {
	int drm_fd = -1;

	// Mode-setting state(connector / CRTC / mode 是一次性 pick 然后固定。)
	uint32_t connector_id = 0;
	uint32_t encoder_id = 0;
	uint32_t crtc_id = 0;
	uint32_t mode_width = 0;
	uint32_t mode_height = 0;
	uint32_t mode_refresh = 0;
	struct _drmModeModeInfo *mode = nullptr; // 拷贝出来的 mode,析构时 memfree。

	struct _drmModeCrtc *original_crtc = nullptr; // 退出前 restore 的原 CRTC 状态。

	// GBM 上下文
	gbm_device *gbm_dev = nullptr;
	gbm_surface *gbm_surf = nullptr;

	// Page-flip 状态
	gbm_bo *current_front_bo = nullptr;
	uint32_t current_front_fb_id = 0;
	bool mode_set = false; // 第一次 page-flip 前要 drmModeSetCrtc 一次。

	// 内部 helper
	Error _pick_connector_crtc_mode(_drmModeRes *resources);
	uint32_t _create_drm_fb(gbm_bo *p_bo);

public:
	Error initialize(const String &p_device_path = "/dev/dri/card0");
	void finalize();

	// 由 EGLManagerKMS 创建 EGL surface 时调
	gbm_device *get_gbm_device() const { return gbm_dev; }
	gbm_surface *get_gbm_surface() const { return gbm_surf; }
	int get_drm_fd() const { return drm_fd; }

	uint32_t get_mode_width() const { return mode_width; }
	uint32_t get_mode_height() const { return mode_height; }

	// 把 GBM front buffer page-flip 到 CRTC。godot 在 eglSwapBuffers 之后立刻调这个。
	// 阻塞等 vblank(同步 vsync);返回 OK = 帧上屏完成。
	Error page_flip();

	KMSGBMDevice() = default;
	~KMSGBMDevice() { finalize(); }
};

#endif // SDL2_ENABLED
