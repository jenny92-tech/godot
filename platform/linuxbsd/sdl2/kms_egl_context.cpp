/**************************************************************************/
/*  kms_egl_context.cpp                                                   */
/**************************************************************************/

#ifdef SDL2_ENABLED

#include "kms_egl_context.h"
#include "kms_config.h"

#include "core/error/error_macros.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"

#include <EGL/eglext.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif
#ifndef EGL_OPENGL_ES2_BIT
#define EGL_OPENGL_ES2_BIT 0x00000004
#endif

static const char *_egl_error_str(EGLint err) {
	switch (err) {
		case EGL_SUCCESS: return "EGL_SUCCESS";
		case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
		case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
		case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
		case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
		case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
		case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
		case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
		case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
		case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
		case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
		case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
		case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
		case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
		case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
		default: return "(unknown)";
	}
}

Error KMSEGLContext::initialize(void *p_gbm_device, void *p_gbm_surface) {
	if (KMSConfig::skip_egl) {
		poc_diag("KMSEGL: POC_SKIP_EGL=1, 跳过 EGL 整体初始化");
		return OK;
	}

	ERR_FAIL_NULL_V(p_gbm_device, ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(p_gbm_surface, ERR_INVALID_PARAMETER);
	poc_diag_fmt("KMSEGL: gbm_device=%p gbm_surface=%p", p_gbm_device, p_gbm_surface);

	// === Step 1: 拿 eglGetPlatformDisplayEXT (扩展函数) ===
	poc_diag("KMSEGL: step1 eglGetProcAddress(eglGetPlatformDisplayEXT)");
	typedef EGLDisplay (*PFNGETPDE)(EGLenum, void *, const EGLint *);
	PFNGETPDE getpde = (PFNGETPDE)eglGetProcAddress("eglGetPlatformDisplayEXT");
	poc_diag_fmt("KMSEGL: getpde ptr = %p", (void *)getpde);

	// === Step 2: 拿 EGL display ===
	if (KMSConfig::egl_use_platform_ext && getpde) {
		poc_diag("KMSEGL: step2 eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm_dev, NULL)");
		egl_display = getpde(EGL_PLATFORM_GBM_KHR, p_gbm_device, nullptr);
	} else {
		poc_diag_fmt("KMSEGL: step2 eglGetDisplay (use_platform_ext=%d, getpde=%p)",
				KMSConfig::egl_use_platform_ext ? 1 : 0, (void *)getpde);
		egl_display = eglGetDisplay((EGLNativeDisplayType)p_gbm_device);
	}
	poc_diag_fmt("KMSEGL: egl_display = %p", (void *)egl_display);

	if (egl_display == EGL_NO_DISPLAY) {
		EGLint err = eglGetError();
		poc_diag_fmt("KMSEGL: FAIL — eglGetDisplay returned NO_DISPLAY, err=0x%x (%s)", err, _egl_error_str(err));
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "eglGetDisplay 返 EGL_NO_DISPLAY");
	}

	// === Step 3: eglInitialize ===
	EGLint major = 0, minor = 0;
	poc_diag("KMSEGL: step3 eglInitialize");
	if (eglInitialize(egl_display, &major, &minor) == EGL_FALSE) {
		EGLint err = eglGetError();
		poc_diag_fmt("KMSEGL: FAIL — eglInitialize err=0x%x (%s)", err, _egl_error_str(err));
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "eglInitialize 失败");
	}
	poc_diag_fmt("KMSEGL: EGL %d.%d initialized", (int)major, (int)minor);

	// === Step 4: eglBindAPI(GLES) ===
	poc_diag("KMSEGL: step4 eglBindAPI(EGL_OPENGL_ES_API)");
	if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
		EGLint err = eglGetError();
		poc_diag_fmt("KMSEGL: FAIL — eglBindAPI err=0x%x (%s)", err, _egl_error_str(err));
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "eglBindAPI 失败");
	}

	// === Step 5: eglChooseConfig (按 config toggle) ===
	const EGLint renderable = (KMSConfig::egl_gles_major >= 3) ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_ES2_BIT;
	const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, renderable,
		EGL_RED_SIZE, KMSConfig::egl_red_size,
		EGL_GREEN_SIZE, KMSConfig::egl_green_size,
		EGL_BLUE_SIZE, KMSConfig::egl_blue_size,
		EGL_ALPHA_SIZE, KMSConfig::egl_alpha_size,
		EGL_DEPTH_SIZE, KMSConfig::egl_depth_size,
		EGL_STENCIL_SIZE, KMSConfig::egl_stencil_size,
		EGL_NONE
	};
	EGLint num_configs = 0;
	poc_diag_fmt("KMSEGL: step5 eglChooseConfig R=%d G=%d B=%d A=%d D=%d S=%d (GLES%d)",
			KMSConfig::egl_red_size, KMSConfig::egl_green_size, KMSConfig::egl_blue_size,
			KMSConfig::egl_alpha_size, KMSConfig::egl_depth_size, KMSConfig::egl_stencil_size,
			KMSConfig::egl_gles_major);
	if (eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) == EGL_FALSE || num_configs < 1) {
		EGLint err = eglGetError();
		poc_diag_fmt("KMSEGL: FAIL — eglChooseConfig num=%d err=0x%x (%s)", num_configs, err, _egl_error_str(err));
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, vformat("eglChooseConfig 失败 (num_configs=%d)", num_configs));
	}
	poc_diag_fmt("KMSEGL: config OK, ptr=%p", (void *)egl_config);

	// === Step 6: eglCreateContext ===
	const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, KMSConfig::egl_gles_major,
		EGL_NONE
	};
	poc_diag_fmt("KMSEGL: step6 eglCreateContext(GLES%d)", KMSConfig::egl_gles_major);
	egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
	if (egl_context == EGL_NO_CONTEXT) {
		EGLint err = eglGetError();
		poc_diag_fmt("KMSEGL: FAIL — eglCreateContext err=0x%x (%s)", err, _egl_error_str(err));
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "eglCreateContext 失败");
	}
	poc_diag_fmt("KMSEGL: context=%p", (void *)egl_context);

	// === Step 7: eglCreateWindowSurface (可 SKIP) ===
	if (KMSConfig::skip_egl_surface) {
		poc_diag("KMSEGL: POC_SKIP_EGL_SURFACE=1, 跳过 createWindowSurface");
	} else {
		poc_diag("KMSEGL: step7 eglCreateWindowSurface(gbm_surface)");
		egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)p_gbm_surface, nullptr);
		if (egl_surface == EGL_NO_SURFACE) {
			EGLint err = eglGetError();
			poc_diag_fmt("KMSEGL: FAIL — eglCreateWindowSurface err=0x%x (%s)", err, _egl_error_str(err));
			ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "eglCreateWindowSurface 失败");
		}
		poc_diag_fmt("KMSEGL: surface=%p", (void *)egl_surface);
	}

	// === Step 8: eglMakeCurrent (可 SKIP) ===
	if (KMSConfig::skip_make_current) {
		poc_diag("KMSEGL: POC_SKIP_MAKE_CURRENT=1, 跳过 eglMakeCurrent");
	} else if (egl_surface != EGL_NO_SURFACE) {
		poc_diag("KMSEGL: step8 eglMakeCurrent");
		if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) == EGL_FALSE) {
			EGLint err = eglGetError();
			poc_diag_fmt("KMSEGL: FAIL — eglMakeCurrent err=0x%x (%s)", err, _egl_error_str(err));
			ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "eglMakeCurrent 失败");
		}
	} else {
		poc_diag("KMSEGL: skip eglMakeCurrent (no surface)");
	}

	vsync_on = (KMSConfig::egl_swap_interval != 0);
	eglSwapInterval(egl_display, KMSConfig::egl_swap_interval);
	poc_diag_fmt("KMSEGL: swap_interval=%d", KMSConfig::egl_swap_interval);

	poc_diag("KMSEGL: ============== ALL EGL INIT DONE ==============");
	print_verbose("KMSEGLContext: 全套 EGL 就绪");
	return OK;
}

void KMSEGLContext::swap_buffers() {
	if (egl_display != EGL_NO_DISPLAY && egl_surface != EGL_NO_SURFACE) {
		if (eglSwapBuffers(egl_display, egl_surface) == EGL_FALSE) {
			poc_diag_fmt("KMSEGL: eglSwapBuffers FAILED err=0x%x", eglGetError());
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
