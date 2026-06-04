/**************************************************************************/
/*  egl_manager_kms.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* EGL Manager subclass for the EGL_KHR_platform_gbm platform — used by   */
/* the SDL2 display server's KMSDRM path on PortMaster handhelds where    */
/* the closed Mali libmali only exposes EGL_KHR_platform_gbm. Mirrors the */
/* EGLManagerWayland pattern but with GBM-specific constants.             */
/**************************************************************************/

#pragma once

#ifdef SDL2_ENABLED
#ifdef EGL_ENABLED
#ifdef GLES3_ENABLED

#include "drivers/egl/egl_manager.h"

class EGLManagerKMS : public EGLManager {
public:
	virtual const char *_get_platform_extension_name() const override;
	virtual EGLenum _get_platform_extension_enum() const override;
	virtual EGLenum _get_platform_api_enum() const override;
	virtual Vector<EGLAttrib> _get_platform_display_attributes() const override;
	virtual Vector<EGLint> _get_platform_context_attribs() const override;
};

#endif // GLES3_ENABLED
#endif // EGL_ENABLED
#endif // SDL2_ENABLED
