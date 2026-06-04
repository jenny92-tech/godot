/**************************************************************************/
/*  kms_gbm_device.cpp                                                    */
/**************************************************************************/

#ifdef SDL2_ENABLED

#include "kms_gbm_device.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"          // memalloc / memfree
#include "core/string/print_string.h"
#include "core/variant/variant.h"     // vformat
#include <cstring>                    // memcpy
#include <cerrno>                     // errno

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

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
	CharString path_utf8 = p_device_path.utf8();
	drm_fd = open(path_utf8.get_data(), O_RDWR | O_CLOEXEC);
	ERR_FAIL_COND_V_MSG(drm_fd < 0, ERR_CANT_OPEN, vformat("KMSGBMDevice: open(%s) failed: %d", p_device_path, errno));

	// 试着拿 DRM master(如果 MainUI 已经 dropMaster)。失败不致命:很多 PortMaster 设备
	// 启动 port 时 MainUI 已经放手,但有的 setup 不放;我们 page-flip 不强制需要 master。
	drmSetMaster(drm_fd);

	drmModeRes *resources = drmModeGetResources(drm_fd);
	ERR_FAIL_NULL_V_MSG(resources, ERR_UNAVAILABLE, "KMSGBMDevice: drmModeGetResources 失败,可能不是 KMS 设备");

	Error err = _pick_connector_crtc_mode(resources);
	drmModeFreeResources(resources);
	if (err != OK) {
		return err;
	}

	// GBM device + surface(直 scanout 用 XRGB8888 + SCANOUT|RENDERING usage)
	gbm_dev = gbm_create_device(drm_fd);
	ERR_FAIL_NULL_V_MSG(gbm_dev, ERR_UNAVAILABLE, "KMSGBMDevice: gbm_create_device 失败");

	gbm_surf = gbm_surface_create(gbm_dev, mode_width, mode_height, GBM_FORMAT_XRGB8888,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	ERR_FAIL_NULL_V_MSG(gbm_surf, ERR_UNAVAILABLE,
			vformat("KMSGBMDevice: gbm_surface_create(%dx%d XRGB8888) 失败", mode_width, mode_height));

	print_verbose(vformat("KMSGBMDevice: GBM surface %dx%d XRGB8888 OK", mode_width, mode_height));
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
	ERR_FAIL_COND_V(gbm_surf == nullptr, ERR_UNCONFIGURED);

	// 拿当前 front buffer(eglSwapBuffers 让它变成 front)
	gbm_bo *next_bo = gbm_surface_lock_front_buffer(gbm_surf);
	ERR_FAIL_NULL_V_MSG(next_bo, ERR_CANT_ACQUIRE_RESOURCE, "KMSGBMDevice: gbm_surface_lock_front_buffer 返 NULL(可能 SwapBuffers 没成功)");

	uint32_t fb_id = _create_drm_fb(next_bo);
	if (!fb_id) {
		gbm_surface_release_buffer(gbm_surf, next_bo);
		return ERR_CANT_CREATE;
	}

	// 第一帧:drmModeSetCrtc(没 SetCrtc,后续 PageFlip 都 -EBUSY)
	if (!mode_set) {
		int ret = drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &connector_id, 1, mode);
		ERR_FAIL_COND_V_MSG(ret, ERR_CANT_CREATE, vformat("KMSGBMDevice: 首次 drmModeSetCrtc 失败: %d", ret));
		mode_set = true;
	} else {
		// 后续帧:page flip + 等 vblank
		bool waiting = true;
		PageFlipUserData pf{ &waiting };
		int ret = drmModePageFlip(drm_fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, &pf);
		if (ret) {
			gbm_surface_release_buffer(gbm_surf, next_bo);
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("KMSGBMDevice: drmModePageFlip 失败: %d", ret));
		}

		// 阻塞等 vblank/page-flip 完成(单一进程,不并发,不复杂)
		struct pollfd pfd = { drm_fd, POLLIN, 0 };
		drmEventContext evctx = {};
		evctx.version = 2;
		evctx.page_flip_handler = _page_flip_handler;
		while (waiting) {
			int pret = poll(&pfd, 1, 1000);
			if (pret <= 0) {
				// 超时或错误:不死循环,放弃这一帧
				WARN_PRINT("KMSGBMDevice: page-flip poll 超时,帧丢");
				break;
			}
			drmHandleEvent(drm_fd, &evctx);
		}
	}

	// 把上一次 front buffer 还给 gbm(它会进 surface 的复用池)
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
