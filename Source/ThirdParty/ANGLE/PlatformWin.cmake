# We're targeting Windows 10 which will have DirectX 11 on it so require that
# but make DirectX 9 optional

list(APPEND ANGLE_DEFINITIONS
    GL_APICALL=
    GL_API=
    NOMINMAX
)

# We're targeting Windows 10 which will have DirectX 11
list(APPEND ANGLE_SOURCES
    ${d3d11_backend_sources}
    ${d3d_shared_sources}

    ${angle_translator_hlsl_sources}

    ${libangle_gpu_info_util_sources}
    ${libangle_gpu_info_util_win_sources}
)

list(APPEND ANGLE_DEFINITIONS
    ANGLE_ENABLE_D3D11
    ANGLE_ENABLE_HLSL
)

list(APPEND ANGLEGLESv2_LIBRARIES dxguid dxgi)

# DirectX 9 support should be optional but ANGLE will not compile without it
list(APPEND ANGLE_SOURCES ${d3d9_backend_sources})
list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_D3D9)
list(APPEND ANGLEGLESv2_LIBRARIES d3d9 synchronization)

# Glass: the GPU process uses GL from several threads on one display (its compositor, and Skia drawing on the GPU in
# each web process's rendering thread). ANGLE's Direct3D 11 backend has one immediate context and one state cache per
# display, so every GL call takes ANGLE's global lock and re-applies a context's state when another one ran in between.
# EGL calls take the same lock (src/libGLESv2/global_state.h), so the lock is recursive.
# Kept off ANGLE_DEFINITIONS, which are public: WebKit's own code doesn't need them.
list(APPEND ANGLE_PRIVATE_DEFINITIONS
    ANGLE_ENABLE_SHARE_CONTEXT_LOCK=1
    ANGLE_FORCE_CONTEXT_CHECK_EVERY_CALL=1
    ANGLE_ENABLE_GLOBAL_MUTEX_RECURSION=1
)

# Use shared libraries
set(GLESv2_LIBRARY_TYPE SHARED)
set(EGL_LIBRARY_TYPE SHARED)
