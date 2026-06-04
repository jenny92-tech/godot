/**************************************************************************/
/*  egl_manager_kms.cpp                                                   */
/**************************************************************************/

#include "egl_manager_kms.h"

#ifdef SDL2_ENABLED
#ifdef EGL_ENABLED
#ifdef GLES3_ENABLED

const char *EGLManagerKMS::_get_platform_extension_name() const {
	return "EGL_KHR_platform_gbm";
}

EGLenum EGLManagerKMS::_get_platform_extension_enum() const {
	return EGL_PLATFORM_GBM_KHR;
}

EGLenum EGLManagerKMS::_get_platform_api_enum() const {
	// 嵌入式 Mali 闭源 driver 几乎只 GLES,不走 desktop GL。godot 的 GLES3 rasterizer 也是 GLES。
	return EGL_OPENGL_ES_API;
}

Vector<EGLAttrib> EGLManagerKMS::_get_platform_display_attributes() const {
	return Vector<EGLAttrib>();
}

Vector<EGLint> EGLManagerKMS::_get_platform_context_attribs() const {
	// GLES 3.0 minimum;部分 Mali 闭源 driver 不挺 3.3,3.0 是最稳妥的下限。
	Vector<EGLint> ret;
	ret.push_back(EGL_CONTEXT_MAJOR_VERSION);
	ret.push_back(3);
	ret.push_back(EGL_CONTEXT_MINOR_VERSION);
	ret.push_back(0);
	ret.push_back(EGL_NONE);
	return ret;
}

#endif // GLES3_ENABLED
#endif // EGL_ENABLED
#endif // SDL2_ENABLED
