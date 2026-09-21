// GeneralsX @build Android port ANGLE integration
// Auto-generated dispatch shim: forwards the 92 gl* entry points d3d8gles
// actually calls to function pointers resolved from a dlopen'd GLES
// implementation (ANGLE's libGLESv2_angle.so, or a system libGLESv3.so
// fallback), instead of linking directly against system libGLESv3.so.
// Android's linker namespace won't let an app-bundled library override a
// public system library of the same name via implicit DT_NEEDED linking,
// so ANGLE's shared libraries must be dlopen'd explicitly under their own
// distinct names (libEGL_angle.so / libGLESv2_angle.so) and every GL entry
// point resolved through this shim instead.
#pragma once

// Loads the given GLES implementation library (by soname, resolved via the
// normal dlopen search path -- e.g. "libGLESv2_angle.so" for the ANGLE
// build bundled in jniLibs) and resolves all gl* pointers used below from
// it. Must be called after the GL context is current (SDL_GL_MakeCurrent)
// and before any gl* call. Returns false (leaving the shim unresolved) if
// the library or any required symbol can't be found -- callers should fall
// back to linking against system GLESv3 in that case.
bool d3d8gles_LoadGLESDispatch(const char *libName);


// GeneralsX @feature visionOS port - resolver-based variant. `getProcAddress` maps a GL
// function name ("glClear", ...) to its address for the CURRENT context; in practice
// eglGetProcAddress of the ANGLE display the host created. The pointers must come from
// the implementation that owns the current context (never mix ANGLE and a system GLES).
// Same contract as d3d8gles_LoadGLESDispatch otherwise: the GL context must already be
// current on the calling thread, and false is returned (leaving the shim partially
// resolved) if any required entry point is missing. Works on every platform.
bool d3d8gles_LoadGLESDispatchFromResolver(void *(*getProcAddress)(const char *name));
