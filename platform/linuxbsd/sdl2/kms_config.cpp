/**************************************************************************/
/*  kms_config.cpp                                                        */
/**************************************************************************/

#ifdef SDL2_ENABLED

#include "kms_config.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>

// 默认值定义
bool KMSConfig::diag_enabled = true;
String KMSConfig::diag_file = "";

String KMSConfig::drm_device = "/dev/dri/card0";
bool KMSConfig::drm_master = true;

uint32_t KMSConfig::gbm_format = GBM_FORMAT_XRGB8888;
bool KMSConfig::gbm_use_linear = false;

bool KMSConfig::egl_use_platform_ext = true;
int KMSConfig::egl_red_size = 8;
int KMSConfig::egl_green_size = 8;
int KMSConfig::egl_blue_size = 8;
int KMSConfig::egl_alpha_size = 0;
int KMSConfig::egl_depth_size = 16;
int KMSConfig::egl_stencil_size = 0;
int KMSConfig::egl_gles_major = 3;
int KMSConfig::egl_swap_interval = 1;

bool KMSConfig::skip_kms = false;
bool KMSConfig::skip_egl = false;
bool KMSConfig::skip_egl_surface = false;
bool KMSConfig::skip_make_current = false;
bool KMSConfig::skip_page_flip = false;
bool KMSConfig::no_vblank_wait = false;

// ===================================================================
// helper: env 取整(默认值兜底)
// ===================================================================
static int _env_int(const char *name, int def) {
	const char *v = getenv(name);
	if (!v || !*v) {
		return def;
	}
	return atoi(v);
}

static bool _env_bool(const char *name, bool def) {
	const char *v = getenv(name);
	if (!v || !*v) {
		return def;
	}
	return atoi(v) != 0;
}

static String _env_str(const char *name, const String &def) {
	const char *v = getenv(name);
	if (!v || !*v) {
		return def;
	}
	return String(v);
}

// GBM format 字符串解析
static uint32_t _parse_gbm_format(const char *s) {
	if (!s || !*s) {
		return GBM_FORMAT_XRGB8888;
	}
	if (strcmp(s, "XRGB") == 0 || strcmp(s, "XRGB8888") == 0) {
		return GBM_FORMAT_XRGB8888;
	}
	if (strcmp(s, "ARGB") == 0 || strcmp(s, "ARGB8888") == 0) {
		return GBM_FORMAT_ARGB8888;
	}
	if (strcmp(s, "XBGR") == 0 || strcmp(s, "XBGR8888") == 0) {
		return GBM_FORMAT_XBGR8888;
	}
	if (strcmp(s, "ABGR") == 0 || strcmp(s, "ABGR8888") == 0) {
		return GBM_FORMAT_ABGR8888;
	}
	if (strcmp(s, "RGB565") == 0) {
		return GBM_FORMAT_RGB565;
	}
	return GBM_FORMAT_XRGB8888;
}

static const char *_gbm_format_name(uint32_t f) {
	switch (f) {
		case GBM_FORMAT_XRGB8888:
			return "XRGB8888";
		case GBM_FORMAT_ARGB8888:
			return "ARGB8888";
		case GBM_FORMAT_XBGR8888:
			return "XBGR8888";
		case GBM_FORMAT_ABGR8888:
			return "ABGR8888";
		case GBM_FORMAT_RGB565:
			return "RGB565";
		default:
			return "(unknown)";
	}
}

// ===================================================================
// load_from_env: 主入口,DSDL2 构造一开始调一次
// ===================================================================
void KMSConfig::load_from_env() {
	// 诊断
	diag_enabled = _env_bool("POC_DIAG", true);
	diag_file = _env_str("POC_DIAG_FILE", "");

	// KMS
	drm_device = _env_str("POC_DRM_DEVICE", "/dev/dri/card0");
	drm_master = _env_bool("POC_DRM_MASTER", true);

	// GBM
	{
		const char *v = getenv("POC_GBM_FORMAT");
		gbm_format = _parse_gbm_format(v);
	}
	gbm_use_linear = _env_bool("POC_GBM_LINEAR", false);

	// EGL
	egl_use_platform_ext = _env_bool("POC_EGL_PLATFORM_EXT", true);
	egl_red_size = _env_int("POC_EGL_RED", 8);
	egl_green_size = _env_int("POC_EGL_GREEN", 8);
	egl_blue_size = _env_int("POC_EGL_BLUE", 8);
	egl_alpha_size = _env_int("POC_EGL_ALPHA", 0);
	egl_depth_size = _env_int("POC_EGL_DEPTH", 16);
	egl_stencil_size = _env_int("POC_EGL_STENCIL", 0);
	egl_gles_major = _env_int("POC_EGL_GLES_VER", 3);
	egl_swap_interval = _env_int("POC_EGL_VSYNC", 1);

	// Skip bisect
	skip_kms = _env_bool("POC_SKIP_KMS", false);
	skip_egl = _env_bool("POC_SKIP_EGL", false);
	skip_egl_surface = _env_bool("POC_SKIP_EGL_SURFACE", false);
	skip_make_current = _env_bool("POC_SKIP_MAKE_CURRENT", false);
	skip_page_flip = _env_bool("POC_SKIP_PAGE_FLIP", false);
	no_vblank_wait = _env_bool("POC_NO_VBLANK_WAIT", false);
}

// ===================================================================
// dump: 把生效配置全打印到 stderr,startup 一眼看出当前 toggle 组合
// ===================================================================
void KMSConfig::dump() {
	char buf[1024];
	int n = snprintf(buf, sizeof(buf),
			"\n[POC-CONFIG] ============== KMSConfig dump ==============\n"
			"[POC-CONFIG] DIAG=%d DIAG_FILE='%s'\n"
			"[POC-CONFIG] DRM_DEVICE='%s' DRM_MASTER=%d\n"
			"[POC-CONFIG] GBM_FORMAT=%s LINEAR=%d\n"
			"[POC-CONFIG] EGL_PLATFORM_EXT=%d RGB=%d/%d/%d A=%d D=%d S=%d GLES=%d VSYNC=%d\n"
			"[POC-CONFIG] SKIP: KMS=%d EGL=%d EGL_SURFACE=%d MAKE_CURRENT=%d PAGE_FLIP=%d NO_VBLANK_WAIT=%d\n"
			"[POC-CONFIG] ==============================================\n\n",
			diag_enabled, diag_file.utf8().get_data(),
			drm_device.utf8().get_data(), drm_master ? 1 : 0,
			_gbm_format_name(gbm_format), gbm_use_linear ? 1 : 0,
			egl_use_platform_ext ? 1 : 0,
			egl_red_size, egl_green_size, egl_blue_size,
			egl_alpha_size, egl_depth_size, egl_stencil_size,
			egl_gles_major, egl_swap_interval,
			skip_kms ? 1 : 0, skip_egl ? 1 : 0, skip_egl_surface ? 1 : 0,
			skip_make_current ? 1 : 0, skip_page_flip ? 1 : 0, no_vblank_wait ? 1 : 0);
	if (n > 0) {
		write(2, buf, n);
	}
}

// ===================================================================
// poc_diag: syscall write 不经 stdio,SIGSEGV 无关,strace 立刻见
// ===================================================================
void poc_diag(const char *step) {
	if (!KMSConfig::diag_enabled) {
		return;
	}
	char buf[256];
	int len = snprintf(buf, sizeof(buf), "[POC-DIAG] %s\n", step);
	if (len > 0) {
		write(2, buf, len);
		if (!KMSConfig::diag_file.is_empty()) {
			int fd = open(KMSConfig::diag_file.utf8().get_data(),
					O_WRONLY | O_APPEND | O_CREAT, 0644);
			if (fd >= 0) {
				write(fd, buf, len);
				close(fd);
			}
		}
	}
}

void poc_diag_fmt(const char *fmt, ...) {
	if (!KMSConfig::diag_enabled) {
		return;
	}
	char buf[512];
	int prefix = snprintf(buf, sizeof(buf), "[POC-DIAG] ");
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + prefix, sizeof(buf) - prefix - 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	int total = prefix + n;
	if (total >= (int)sizeof(buf) - 1) {
		total = sizeof(buf) - 2;
	}
	buf[total] = '\n';
	buf[total + 1] = '\0';
	write(2, buf, total + 1);
	if (!KMSConfig::diag_file.is_empty()) {
		int fd = open(KMSConfig::diag_file.utf8().get_data(),
				O_WRONLY | O_APPEND | O_CREAT, 0644);
		if (fd >= 0) {
			write(fd, buf, total + 1);
			close(fd);
		}
	}
}

#endif // SDL2_ENABLED
