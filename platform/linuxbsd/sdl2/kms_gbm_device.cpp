/**************************************************************************/
/*  kms_gbm_device.cpp                                                    */
/**************************************************************************/

#ifdef SDL2_ENABLED

#include "kms_gbm_device.h"
#include "kms_config.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"          // memalloc / memfree
#include "core/string/print_string.h"
#include "core/variant/variant.h"     // vformat
#include <cstring>                    // memcpy
#include <cerrno>                     // errno

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// ── Crash/exit DRM restore ───────────────────────────────────────────────
// On the KMS-direct path godot is the DRM master driving the panel. If it
// crashes (SIGSEGV/SIGABRT/SIGBUS) or is killed (SIGTERM/SIGINT), the normal
// finalize() never runs: the VOP keeps scanning out a GBM buffer whose memory
// is freed as the process dies → on RK3566 the kernel watchdog REBOOTS the
// whole device. We arm a signal handler that restores the panel's original
// CRTC first, so a crash just exits (no reboot). POD statics only — the handler
// must not touch C++ objects or allocate.
static int g_kms_restore_fd = -1;
static uint32_t g_kms_restore_crtc_id = 0;
static uint32_t g_kms_restore_buffer_id = 0;
static uint32_t g_kms_restore_connector_id = 0;
static int g_kms_restore_x = 0;
static int g_kms_restore_y = 0;
static drmModeModeInfo g_kms_restore_mode = {};
static volatile sig_atomic_t g_kms_restore_armed = 0;

static void _kms_crash_restore(int sig) {
	if (g_kms_restore_armed && g_kms_restore_fd >= 0) {
		g_kms_restore_armed = 0; // avoid re-entry
		drmModeSetCrtc(g_kms_restore_fd, g_kms_restore_crtc_id, g_kms_restore_buffer_id,
				g_kms_restore_x, g_kms_restore_y, &g_kms_restore_connector_id, 1, &g_kms_restore_mode);
	}
	// Re-raise with the default handler so the process actually dies (core dump
	// for SIGSEGV/SIGABRT, normal termination for SIGTERM/SIGINT).
	signal(sig, SIG_DFL);
	raise(sig);
}

// ===================================================================
// 找一个 connected connector + 它的 encoder + CRTC + 一个 mode
// ===================================================================
Error KMSGBMDevice::_pick_connector_crtc_mode(drmModeRes *resources) {
	// 1) 找一个 connected 的 connector
	drmModeConnector *picked_connector = nullptr;
	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(drm_fd, resources->connectors[i]);
		if (!c) {
			continue;
		}
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			picked_connector = c;
			break;
		}
		drmModeFreeConnector(c);
	}
	ERR_FAIL_NULL_V_MSG(picked_connector, ERR_UNAVAILABLE, "KMSGBMDevice: 没有 connected connector(/dev/dri/card0 上无显示输出?)");

	connector_id = picked_connector->connector_id;

	// 2) 选一个 mode:优先 preferred(panel native),否则第一个
	drmModeModeInfo *picked_mode = nullptr;
	for (int m = 0; m < picked_connector->count_modes; m++) {
		drmModeModeInfo *mi = &picked_connector->modes[m];
		if (mi->type & DRM_MODE_TYPE_PREFERRED) {
			picked_mode = mi;
			break;
		}
	}
	if (!picked_mode) {
		picked_mode = &picked_connector->modes[0];
	}

	// 拷贝出来(后面 drmModeFreeConnector 会释放原 mode 数组)
	mode = (drmModeModeInfo *)memalloc(sizeof(drmModeModeInfo));
	memcpy(mode, picked_mode, sizeof(drmModeModeInfo));
	mode_width = mode->hdisplay;
	mode_height = mode->vdisplay;
	mode_refresh = mode->vrefresh;

	// 3) 找 encoder + CRTC
	// 先试 connector 的 encoder_id(如果它已连过 encoder)
	if (picked_connector->encoder_id) {
		drmModeEncoder *enc = drmModeGetEncoder(drm_fd, picked_connector->encoder_id);
		if (enc) {
			encoder_id = enc->encoder_id;
			crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
	}
	// 退路:遍历所有 encoders 找一个 connector 可用的
	if (!crtc_id) {
		for (int i = 0; i < picked_connector->count_encoders; i++) {
			drmModeEncoder *enc = drmModeGetEncoder(drm_fd, picked_connector->encoders[i]);
			if (!enc) {
				continue;
			}
			// 找一个 enc 的 possible_crtcs bitmask 里第一个可用 CRTC
			for (int k = 0; k < resources->count_crtcs; k++) {
				if (enc->possible_crtcs & (1u << k)) {
					encoder_id = enc->encoder_id;
					crtc_id = resources->crtcs[k];
					break;
				}
			}
			drmModeFreeEncoder(enc);
			if (crtc_id) {
				break;
			}
		}
	}

	drmModeFreeConnector(picked_connector);
	ERR_FAIL_COND_V_MSG(crtc_id == 0, ERR_UNAVAILABLE, "KMSGBMDevice: connector 找到但配不出 CRTC");

	// 备份当前 CRTC 状态(以便退出 restore)
	original_crtc = drmModeGetCrtc(drm_fd, crtc_id);

	// Arm the crash/exit DRM-restore signal handler (see top of file). Without
	// this a godot crash on the KMS-direct path reboots the whole device.
	if (original_crtc) {
		g_kms_restore_fd = drm_fd;
		g_kms_restore_crtc_id = original_crtc->crtc_id;
		g_kms_restore_buffer_id = original_crtc->buffer_id;
		g_kms_restore_x = original_crtc->x;
		g_kms_restore_y = original_crtc->y;
		g_kms_restore_mode = original_crtc->mode;
		g_kms_restore_connector_id = connector_id;
		g_kms_restore_armed = 1;

		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = _kms_crash_restore;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		// Only the true CRASH signals — leave SIGTERM/SIGINT to godot's own clean
		// quit path (which runs finalize() → restores the CRTC anyway).
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGABRT, &sa, nullptr);
		sigaction(SIGBUS, &sa, nullptr);
		sigaction(SIGFPE, &sa, nullptr);
		sigaction(SIGILL, &sa, nullptr);
	}

	print_verbose(vformat("KMSGBMDevice: connector %d, CRTC %d, mode %dx%d@%d", connector_id, crtc_id, mode_width, mode_height, mode_refresh));
	return OK;
}

// ===================================================================
// 把一个 GBM buffer object 注册成 DRM framebuffer
// ===================================================================
uint32_t KMSGBMDevice::_create_drm_fb(gbm_bo *p_bo) {
	// 如果之前已绑过(BO 在 gbm_surface 里 round-robin 复用),复用 fb_id
	uint32_t fb_id = (uint32_t)(uintptr_t)gbm_bo_get_user_data(p_bo);
	if (fb_id) {
		return fb_id;
	}

	uint32_t width = gbm_bo_get_width(p_bo);
	uint32_t height = gbm_bo_get_height(p_bo);
	uint32_t format = gbm_bo_get_format(p_bo);
	uint32_t handles[4] = { gbm_bo_get_handle(p_bo).u32, 0, 0, 0 };
	uint32_t strides[4] = { gbm_bo_get_stride(p_bo), 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };

	int ret = drmModeAddFB2(drm_fd, width, height, format, handles, strides, offsets, &fb_id, 0);
	if (ret) {
		// 旧 kernel 没 AddFB2 → 退回 AddFB
		ret = drmModeAddFB(drm_fd, width, height, 24, 32, strides[0], handles[0], &fb_id);
	}
	ERR_FAIL_COND_V_MSG(ret, 0, vformat("KMSGBMDevice: drmModeAddFB(2) failed: %d", ret));

	// 存 fb_id 到 BO 的 user_data,destroy 时 free(BO 是 gbm_surface 复用的池子里的)
	gbm_bo_set_user_data(p_bo, (void *)(uintptr_t)fb_id, [](gbm_bo *bo, void *data) {
		// gbm 在 surface destroy / release 时回调
		drmModeRmFB(gbm_device_get_fd(gbm_bo_get_device(bo)), (uint32_t)(uintptr_t)data);
	});

	return fb_id;
}

// ===================================================================
// initialize:open card + 找 connector/CRTC/mode + 创 gbm_device + gbm_surface
// ===================================================================
Error KMSGBMDevice::initialize(const String &p_device_path) {
	String dev = KMSConfig::drm_device.is_empty() ? p_device_path : KMSConfig::drm_device;
	poc_diag_fmt("KMSGBM: open(%s)", dev.utf8().get_data());

	CharString path_utf8 = dev.utf8();
	drm_fd = open(path_utf8.get_data(), O_RDWR | O_CLOEXEC);
	ERR_FAIL_COND_V_MSG(drm_fd < 0, ERR_CANT_OPEN, vformat("KMSGBMDevice: open(%s) failed: %d", dev, errno));
	poc_diag_fmt("KMSGBM: open OK, fd=%d", drm_fd);

	if (KMSConfig::drm_master) {
		poc_diag("KMSGBM: drmSetMaster()");
		int r = drmSetMaster(drm_fd);
		poc_diag_fmt("KMSGBM: drmSetMaster returned %d (errno=%d)", r, r ? errno : 0);
	} else {
		poc_diag("KMSGBM: skip drmSetMaster (POC_DRM_MASTER=0)");
	}

	poc_diag("KMSGBM: drmModeGetResources()");
	drmModeRes *resources = drmModeGetResources(drm_fd);
	ERR_FAIL_NULL_V_MSG(resources, ERR_UNAVAILABLE, "KMSGBMDevice: drmModeGetResources 失败,可能不是 KMS 设备");

	poc_diag("KMSGBM: _pick_connector_crtc_mode()");
	Error err = _pick_connector_crtc_mode(resources);
	drmModeFreeResources(resources);
	if (err != OK) {
		poc_diag("KMSGBM: pick failed");
		return err;
	}
	poc_diag_fmt("KMSGBM: picked connector=%u crtc=%u mode=%ux%u@%u",
			connector_id, crtc_id, mode_width, mode_height, mode_refresh);

	poc_diag("KMSGBM: gbm_create_device()");
	gbm_dev = gbm_create_device(drm_fd);
	ERR_FAIL_NULL_V_MSG(gbm_dev, ERR_UNAVAILABLE, "KMSGBMDevice: gbm_create_device 失败");
	poc_diag_fmt("KMSGBM: gbm_dev=%p", (void *)gbm_dev);

	uint32_t usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
	if (KMSConfig::gbm_use_linear) {
		usage |= GBM_BO_USE_LINEAR;
	}
	poc_diag_fmt("KMSGBM: gbm_surface_create(%ux%u format=0x%x usage=0x%x)",
			mode_width, mode_height, KMSConfig::gbm_format, usage);
	gbm_surf = gbm_surface_create(gbm_dev, mode_width, mode_height,
			KMSConfig::gbm_format, usage);
	ERR_FAIL_NULL_V_MSG(gbm_surf, ERR_UNAVAILABLE,
			vformat("KMSGBMDevice: gbm_surface_create(%dx%d fmt=0x%x) 失败", mode_width, mode_height, KMSConfig::gbm_format));
	poc_diag_fmt("KMSGBM: gbm_surf=%p", (void *)gbm_surf);

	print_verbose(vformat("KMSGBMDevice: GBM surface %dx%d OK", mode_width, mode_height));
	return OK;
}

// ===================================================================
// page_flip:把刚 eglSwapBuffers 出来的 front buffer 上屏
// ===================================================================
struct PageFlipUserData {
	bool *waiting;
};

static void _page_flip_handler(int /*fd*/, unsigned int /*sequence*/, unsigned int /*tv_sec*/, unsigned int /*tv_usec*/, void *user_data) {
	PageFlipUserData *pf = (PageFlipUserData *)user_data;
	if (pf && pf->waiting) {
		*pf->waiting = false;
	}
}

Error KMSGBMDevice::page_flip() {
	if (KMSConfig::skip_page_flip) {
		return OK; // launcher 设了 POC_SKIP_PAGE_FLIP,只测渲染不上屏
	}
	ERR_FAIL_COND_V(gbm_surf == nullptr, ERR_UNCONFIGURED);

	gbm_bo *next_bo = gbm_surface_lock_front_buffer(gbm_surf);
	ERR_FAIL_NULL_V_MSG(next_bo, ERR_CANT_ACQUIRE_RESOURCE, "KMSGBMDevice: gbm_surface_lock_front_buffer 返 NULL");

	uint32_t fb_id = _create_drm_fb(next_bo);
	if (!fb_id) {
		gbm_surface_release_buffer(gbm_surf, next_bo);
		return ERR_CANT_CREATE;
	}

	if (!mode_set) {
		int ret = drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &connector_id, 1, mode);
		ERR_FAIL_COND_V_MSG(ret, ERR_CANT_CREATE, vformat("KMSGBMDevice: 首次 drmModeSetCrtc 失败: %d", ret));
		mode_set = true;
	} else {
		bool waiting = true;
		PageFlipUserData pf{ &waiting };
		int ret = drmModePageFlip(drm_fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, &pf);
		if (ret) {
			gbm_surface_release_buffer(gbm_surf, next_bo);
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("KMSGBMDevice: drmModePageFlip 失败: %d", ret));
		}

		if (!KMSConfig::no_vblank_wait) {
			struct pollfd pfd = { drm_fd, POLLIN, 0 };
			drmEventContext evctx = {};
			evctx.version = 2;
			evctx.page_flip_handler = _page_flip_handler;
			while (waiting) {
				int pret = poll(&pfd, 1, 1000);
				if (pret <= 0) {
					WARN_PRINT("KMSGBMDevice: page-flip poll 超时,帧丢");
					break;
				}
				drmHandleEvent(drm_fd, &evctx);
			}
		}
	}

	if (current_front_bo) {
		gbm_surface_release_buffer(gbm_surf, current_front_bo);
	}
	current_front_bo = next_bo;
	current_front_fb_id = fb_id;

	return OK;
}

// ===================================================================
// finalize:还原 CRTC + 释放 GBM/DRM 资源
// ===================================================================
void KMSGBMDevice::finalize() {
	// Clean exit: disarm the crash handler first (it would otherwise restore a
	// now-freed CRTC) and reset signals to default.
	g_kms_restore_armed = 0;
	signal(SIGSEGV, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGILL, SIG_DFL);

	// 还原 CRTC(让 MainUI 等系统压回去能直接接管)
	if (original_crtc && drm_fd >= 0) {
		drmModeSetCrtc(drm_fd, original_crtc->crtc_id, original_crtc->buffer_id,
				original_crtc->x, original_crtc->y,
				&connector_id, 1, &original_crtc->mode);
		drmModeFreeCrtc(original_crtc);
		original_crtc = nullptr;
	}

	if (current_front_bo && gbm_surf) {
		gbm_surface_release_buffer(gbm_surf, current_front_bo);
		current_front_bo = nullptr;
	}

	if (gbm_surf) {
		gbm_surface_destroy(gbm_surf);
		gbm_surf = nullptr;
	}
	if (gbm_dev) {
		gbm_device_destroy(gbm_dev);
		gbm_dev = nullptr;
	}

	if (mode) {
		memfree(mode);
		mode = nullptr;
	}

	if (drm_fd >= 0) {
		drmDropMaster(drm_fd);
		close(drm_fd);
		drm_fd = -1;
	}
}

#endif // SDL2_ENABLED
