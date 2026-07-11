/**************************************************************************/
/*  kms_egl_context.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* 最小化 EGL 引导,专给 KMS+GBM 平台用。                                  */
/*                                                                        */
/* 跳过 godot 自家 EGLManager 的 probe→load→terminate→recreate 模式 ——    */
/* 那套模式给闭源 Mali libmali 2 倍接触面、2 倍 NULL bug 机会。我们直接   */
/* 做最少 EGL 调用:GetDisplay → Initialize → BindAPI → ChooseConfig →    */
/* CreateContext → CreateWindowSurface → MakeCurrent → SwapBuffers。      */
/*                                                                        */
/* FRT 4.1.3 binary 经实测在吹米闭源 libmali 上跑得通,采用的就是这套     */
/* 直 libEGL 套路(NEEDED 链 libEGL.so.1)。                              */
/**************************************************************************/

#pragma once

#ifdef SDL2_ENABLED

#include "core/error/error_list.h"

#include <EGL/egl.h>

class KMSEGLContext {
	EGLDisplay egl_display = EGL_NO_DISPLAY;
	EGLContext egl_context = EGL_NO_CONTEXT;
	EGLSurface egl_surface = EGL_NO_SURFACE;
	EGLConfig egl_config = nullptr;

	bool vsync_on = true;

public:
	// 初始化整套 EGL:把 gbm_device 给 libEGL,创 GLES3 context,在 gbm_surface 上建 EGL surface。
	// 调用前 KMSGBMDevice 必须已 init(gbm_device + gbm_surface 都有)。
	Error initialize(void *p_gbm_device, void *p_gbm_surface);

	// godot 调:DisplayServer::swap_buffers → 我们 → eglSwapBuffers(让 GBM front buffer 准备好)
	// 注意:eglSwapBuffers 之后调用方(DSDL2)还要 kms_dev->page_flip() 把 buffer 上屏。
	void swap_buffers();

	void make_current();
	void set_vsync(bool p_enable);
	bool is_vsync() const { return vsync_on; }

	EGLDisplay get_display() const { return egl_display; }
	EGLContext get_context() const { return egl_context; }

	// 给 godot gles3 driver 用:它会调 eglGetProcAddress 拿 GL 函数指针,
	// 我们已经把 libEGL 链好了(SCsub `-lEGL -lGLESv2`),它直接调就行。

	void finalize();

	KMSEGLContext() = default;
	~KMSEGLContext() { finalize(); }
};

#endif // SDL2_ENABLED
