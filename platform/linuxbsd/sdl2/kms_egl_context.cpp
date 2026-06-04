/**************************************************************************/
/*  kms_egl_context.cpp                                                   */
/**************************************************************************/

#ifdef SDL2_ENABLED

#include "kms_egl_context.h"

#include "core/error/error_macros.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"

#include <EGL/eglext.h>

// 部分老 EGL header 没带 GBM 平台常量。Khronos 标准 token 0x31D7。
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

// ===================================================================
// 工具:把 EGL 错误码转成可读字符串(出错时打 log 用)
// ===================================================================
static const char *_egl_error_str(EGLint err) {
	switch (err) {
		case EGL_SUCCESS:
			return "EGL_SUCCESS";
		case EGL_NOT_INITIALIZED:
			return "EGL_NOT_INITIALIZED";
		case EGL_BAD_ACCESS:
			return "EGL_BAD_ACCESS";
		case EGL_BAD_ALLOC:
			return "EGL_BAD_ALLOC";
		case EGL_BAD_ATTRIBUTE:
			return "EGL_BAD_ATTRIBUTE";
		case EGL_BAD_CONFIG:
			return "EGL_BAD_CONFIG";
		case EGL_BAD_CONTEXT:
			return "EGL_BAD_CONTEXT";
		case EGL_BAD_CURRENT_SURFACE:
			return "EGL_BAD_CURRENT_SURFACE";
		case EGL_BAD_DISPLAY:
			return "EGL_BAD_DISPLAY";
		case EGL_BAD_MATCH:
			return "EGL_BAD_MATCH";
		case EGL_BAD_NATIVE_PIXMAP:
			return "EGL_BAD_NATIVE_PIXMAP";
		case EGL_BAD_NATIVE_WINDOW:
			return "EGL_BAD_NATIVE_WINDOW";
		case EGL_BAD_PARAMETER:
			return "EGL_BAD_PARAMETER";
		case EGL_BAD_SURFACE:
			return "EGL_BAD_SURFACE";
		case EGL_CONTEXT_LOST:
			return "EGL_CONTEXT_LOST";
		default:
			return "(unknown)";
	}
}

#define EGL_LOG_STEP(step) print_verbose(vformat("KMSEGLContext: %s", step))
#define EGL_LOG_ERR(step) print_verbose(vformat("KMSEGLContext: %s FAILED, eglGetError=0x%x (%s)", step, eglGetError(), _egl_error_str(eglGetError())))

// ===================================================================
// initialize:最小化 EGL 初始化序列
// ===================================================================
Error KMSEGLContext::initialize(void *p_gbm_device, void *p_gbm_surface) {
	ERR_FAIL_NULL_V(p_gbm_device, ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(p_gbm_surface, ERR_INVALID_PARAMETER);

	// === Step 1:eglGetPlatformDisplayEXT 拿 EGL display(用 GBM 平台扩展)===
	// 通过 eglGetProcAddress 拿扩展函数指针(NEEDED 的 libEGL 只导出 1.0 基础函数)
	EGL_LOG_STEP("step 1: getting eglGetPlatformDisplayEXT");
	typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXT)(EGLenum, void *, const EGLint *);
	PFNEGLGETPLATFORMDISPLAYEXT eglGetPlatformDisplayEXT_p =
			(PFNEGLGETPLATFORMDISPLAYEXT)eglGetProcAddress("eglGetPlatformDisplayEXT");

	if (eglGetPlatformDisplayEXT_p) {
		EGL_LOG_STEP("step 2: eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm_dev)");
		egl_display = eglGetPlatformDisplayEXT_p(EGL_PLATFORM_GBM_KHR, p_gbm_device, nullptr);
	} else {
		// 退路:Mali 老 libmali 没扩展时,直接 eglGetDisplay(gbm_device 强转)
		EGL_LOG_STEP("step 2 (fallback): eglGetDisplay(gbm_device)");
		egl_display = eglGetDisplay((EGLNativeDisplayType)p_gbm_device);
	}

	if (egl_display == EGL_NO_DISPLAY) {
		EGL_LOG_ERR("eglGetDisplay");
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "KMSEGLContext: eglGetDisplay 返 EGL_NO_DISPLAY");
	}

	// === Step 3:eglInitialize ===
	EGLint major = 0, minor = 0;
	EGL_LOG_STEP("step 3: eglInitialize");
	if (eglInitialize(egl_display, &major, &minor) == EGL_FALSE) {
		EGL_LOG_ERR("eglInitialize");
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "KMSEGLContext: eglInitialize 失败");
	}
	print_verbose(vformat("KMSEGLContext: EGL %d.%d initialized", (int)major, (int)minor));

	// === Step 4:eglBindAPI(GLES) ===
	EGL_LOG_STEP("step 4: eglBindAPI(EGL_OPENGL_ES_API)");
	if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
		EGL_LOG_ERR("eglBindAPI");
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "KMSEGLContext: eglBindAPI(GLES) 失败");
	}

	// === Step 5:eglChooseConfig — 最保守配置 ===
	const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 0, // 不要 alpha;吹米面板 XRGB8888,无 alpha
		EGL_DEPTH_SIZE, 16,
		EGL_STENCIL_SIZE, 0,
		EGL_NONE
	};

	EGLint num_configs = 0;
	EGL_LOG_STEP("step 5: eglChooseConfig");
	if (eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) == EGL_FALSE || num_configs < 1) {
		EGL_LOG_ERR("eglChooseConfig");
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, vformat("KMSEGLContext: eglChooseConfig 失败 (num_configs=%d)", num_configs));
	}

	// === Step 6:eglCreateContext (GLES 3.0) ===
	const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_NONE
	};
	EGL_LOG_STEP("step 6: eglCreateContext(GLES3)");
	egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
	if (egl_context == EGL_NO_CONTEXT) {
		EGL_LOG_ERR("eglCreateContext");
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "KMSEGLContext: eglCreateContext 失败");
	}

	// === Step 7:eglCreateWindowSurface(用 gbm_surface)===
	EGL_LOG_STEP("step 7: eglCreateWindowSurface(gbm_surface)");
	egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)p_gbm_surface, nullptr);
	if (egl_surface == EGL_NO_SURFACE) {
		EGL_LOG_ERR("eglCreateWindowSurface");
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "KMSEGLContext: eglCreateWindowSurface 失败");
	}

	// === Step 8:eglMakeCurrent — 把 context 跟 surface 绑了,godot gles3 driver 自此能用 GL ===
	EGL_LOG_STEP("step 8: eglMakeCurrent");
	if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) == EGL_FALSE) {
		EGL_LOG_ERR("eglMakeCurrent");
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "KMSEGLContext: eglMakeCurrent 失败");
	}

	// vsync(swap interval 1)
	eglSwapInterval(egl_display, vsync_on ? 1 : 0);

	print_verbose("KMSEGLContext: 全套 EGL 就绪,GLES3 context 已 current");
	return OK;
}

void KMSEGLContext::swap_buffers() {
	if (egl_display != EGL_NO_DISPLAY && egl_surface != EGL_NO_SURFACE) {
		if (eglSwapBuffers(egl_display, egl_surface) == EGL_FALSE) {
			print_verbose(vformat("KMSEGLContext: eglSwapBuffers FAILED, err=0x%x", eglGetError()));
		}
	}
}

void KMSEGLContext::make_current() {
	if (egl_display != EGL_NO_DISPLAY && egl_context != EGL_NO_CONTEXT && egl_surface != EGL_NO_SURFACE) {
		eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
	}
}

void KMSEGLContext::set_vsync(bool p_enable) {
	vsync_on = p_enable;
	if (egl_display != EGL_NO_DISPLAY) {
		eglSwapInterval(egl_display, vsync_on ? 1 : 0);
	}
}

void KMSEGLContext::finalize() {
	if (egl_display != EGL_NO_DISPLAY) {
		eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (egl_surface != EGL_NO_SURFACE) {
			eglDestroySurface(egl_display, egl_surface);
			egl_surface = EGL_NO_SURFACE;
		}
		if (egl_context != EGL_NO_CONTEXT) {
			eglDestroyContext(egl_display, egl_context);
			egl_context = EGL_NO_CONTEXT;
		}
		eglTerminate(egl_display);
		egl_display = EGL_NO_DISPLAY;
	}
	egl_config = nullptr;
}

#endif // SDL2_ENABLED
