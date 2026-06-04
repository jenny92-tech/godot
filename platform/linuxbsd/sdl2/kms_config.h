/**************************************************************************/
/*  kms_config.h                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* POC 调试期间的全套环境变量 toggle:不同设备 / 不同 Mali 闭源 driver 版 */
/* 本 / 不同内核 KMS 实现下,我们的 KMS+GBM+EGL stack 有些选项需要可调。  */
/* 比起每次猜测后再重编(20 分钟一轮),这一版把所有"我能想到的"参数都   */
/* 暴露成 env var,真机上 launcher 里改 env 来回切。                       */
/*                                                                        */
/* 启动时全 dump 到 stderr(syscall write,SIGSEGV 也丢不了)。            */
/* 每个关键步骤 diag(),strace + stderr 双输出,精确定位崩点。            */
/**************************************************************************/

#pragma once

#ifdef SDL2_ENABLED

#include "core/string/ustring.h"

class KMSConfig {
public:
	// ===== 诊断 =====
	static bool diag_enabled;
	static String diag_file; // 非空 = 同时写文件(SIGSEGV 安全)

	// ===== KMS / DRM =====
	static String drm_device;   // 哪张卡(default /dev/dri/card0)
	static bool drm_master;     // 抢 drmSetMaster?(default true)

	// ===== GBM 表面 =====
	static uint32_t gbm_format; // GBM_FORMAT_XRGB8888 / ARGB / XBGR / RGB565
	static bool gbm_use_linear; // 加 GBM_BO_USE_LINEAR usage(老 Mali 不支持 modifier)

	// ===== EGL 初始化 =====
	static bool egl_use_platform_ext; // eglGetPlatformDisplayEXT vs eglGetDisplay
	static int egl_red_size;
	static int egl_green_size;
	static int egl_blue_size;
	static int egl_alpha_size;
	static int egl_depth_size;
	static int egl_stencil_size;
	static int egl_gles_major;        // GLES 2 or 3
	static int egl_swap_interval;     // 0 = no vsync, 1 = vsync

	// ===== 增量 bisect skip(逐步关掉某层,看 godot 在哪里崩)=====
	static bool skip_kms;             // 完全跳过 KMS+EGL(只 SDL2 dummy,测 godot setup 本身)
	static bool skip_egl;             // KMS OK,但不创 EGL(测 KMS 是否单独 OK)
	static bool skip_egl_surface;     // EGL display+init OK,不创 surface
	static bool skip_make_current;    // 创 surface 但不 eglMakeCurrent
	static bool skip_page_flip;       // swap_buffers 不调 drmModePageFlip(只 eglSwapBuffers)
	static bool no_vblank_wait;       // PageFlip 不阻塞等 vblank(测 deadlock)

	// 一次读全 env,启动时调
	static void load_from_env();
	// dump 到 stderr(syscall write,startup 见)
	static void dump();
};

// 诊断:每个关键步骤打 [POC-DIAG] 前缀的 step 字符串。
// 用 write(2,...) 直接 syscall,跟 stdio buffer 无关,SIGSEGV 时不丢。
// 同时若 KMSConfig::diag_file 非空,也 append 到文件(双保险)。
void poc_diag(const char *step);
void poc_diag_fmt(const char *fmt, ...); // 带变量的版本(内部 snprintf)

#endif // SDL2_ENABLED
