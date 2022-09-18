
#include <iostream>
#include <memory>
#include <complex>

#include <CL/sycl.hpp>

#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client.h"

#include <EGL/egl.h> // must be included below wayland-egl.h...
#include <GLES3/gl3.h>

inline auto safe_ptr(wl_display* display) noexcept {
    return std::unique_ptr<wl_display,
                           decltype (&wl_display_disconnect)>(display,
                                                              wl_display_disconnect);
}
#define INTERN_SAFE_PTR(wl_client)                                      \
    inline auto safe_ptr(wl_client* ptr) noexcept {                     \
        return std::unique_ptr<wl_client,                               \
                               decltype(&wl_client##_destroy)>(ptr,     \
                                                               wl_client##_destroy); \
    }
INTERN_SAFE_PTR(wl_registry)
INTERN_SAFE_PTR(wl_compositor)
INTERN_SAFE_PTR(wl_seat)
INTERN_SAFE_PTR(wl_surface)
INTERN_SAFE_PTR(zxdg_shell_v6)
INTERN_SAFE_PTR(zxdg_surface_v6)
INTERN_SAFE_PTR(zxdg_toplevel_v6)
INTERN_SAFE_PTR(wl_egl_window)
INTERN_SAFE_PTR(wl_keyboard)
INTERN_SAFE_PTR(wl_pointer)
INTERN_SAFE_PTR(wl_touch)

template <class T, class D>
inline auto safe_ptr(T* ptr, D deleter) {
    return std::unique_ptr<T, D>(ptr, deleter);
}

int main() {
    auto display = safe_ptr(wl_display_connect(nullptr));
    if (!display) {
        std::cerr << "wl_display_connect failed..." << std::endl;
        return -1;
    }
    auto registry = safe_ptr(wl_display_get_registry(display.get()));
    if (!registry) {
        std::cerr << "wl_display_get_display failed..." << std::endl;
        return -1;
    }
    auto compositor = safe_ptr((wl_compositor*)nullptr);
    auto shell = safe_ptr((zxdg_shell_v6*) nullptr);
    auto seat = safe_ptr((wl_seat*) nullptr);
    auto registry_global = [&](auto name, std::string_view interface, auto version) noexcept {
        if (interface == wl_compositor_interface.name) {
            compositor.reset(
                reinterpret_cast<wl_compositor*>(wl_registry_bind(registry.get(),
                                                                  name,
                                                                  &wl_compositor_interface,
                                                                  version)));
        }
        else if (interface == zxdg_shell_v6_interface.name) {
            shell.reset(reinterpret_cast<zxdg_shell_v6*>(wl_registry_bind(registry.get(),
                                                                          name,
                                                                          &zxdg_shell_v6_interface,
                                                                          version)));
        }
        else if (interface == wl_seat_interface.name) {
            seat.reset(reinterpret_cast<wl_seat*>(wl_registry_bind(registry.get(),
                                                                   name,
                                                                   &wl_seat_interface,
                                                                   version)));
        }
    };
    wl_registry_listener registry_listener {
        .global = [](auto data, auto, auto... args) noexcept {
            (*reinterpret_cast<decltype (registry_global)*>(data))(args...);
        },
        .global_remove = [](auto...) noexcept { },
    };
    if (wl_registry_add_listener(registry.get(), &registry_listener, &registry_global) != 0) {
        std::cerr << "wl_register_add_listener failed..." << std::endl;
        return -1;
    }
    if (wl_display_roundtrip(display.get()) == -1) {
        std::cerr << "wl_display_roundtrip failed..." << std::endl;
        return -1;
    }
    if (!compositor || !shell || !seat) {
        std::cerr << "Some required global not found..." << std::endl;
        return -1;
    }
    zxdg_shell_v6_listener shell_listener {
        .ping = [](auto, auto shell, auto serial) noexcept {
            zxdg_shell_v6_pong(shell, serial);
        },
    };
    if (zxdg_shell_v6_add_listener(shell.get(), &shell_listener, nullptr) != 0) {
        std::cerr << "zxdg_shell_v6_add_listener failed..." << std::endl;
        return -1;
    }
    auto surface = safe_ptr(wl_compositor_create_surface(compositor.get()));
    if (!surface) {
        std::cerr << "wl_compositor_create_surface failed..." << std::endl;
        return -1;
    }
    auto xdg_surface = safe_ptr(zxdg_shell_v6_get_xdg_surface(shell.get(), surface.get()));
    if (!xdg_surface) {
        std::cerr << "zxdg_shell_v6_get_xdg_surface failed..." << std::endl;
        return -1;
    }
    zxdg_surface_v6_listener xdg_surface_listener {
        .configure = [](auto, auto xdg_surface, auto serial) noexcept {
            zxdg_surface_v6_ack_configure(xdg_surface, serial);
        }
    };
    if (zxdg_surface_v6_add_listener(xdg_surface.get(), &xdg_surface_listener, nullptr) != 0) {
        std::cerr << "zxdg_surface_v6_add_listener failed..." << std::endl;
        return -1;
    }
    auto toplevel = safe_ptr(zxdg_surface_v6_get_toplevel(xdg_surface.get()));
    if (!toplevel) {
        std::cerr << "zxdg_surface_v6_get_toplevel failed..." << std::endl;
        return -1;
    }
    float resolution_vec[2] = { 640, 480 };
    auto egl_display = safe_ptr(eglGetDisplay(display.get()), eglTerminate);
    if (!egl_display) {
        std::cerr << "eglGetDisplay failed..." << std::endl;
        return -1;
    }
    if (!eglInitialize(egl_display.get(), nullptr, nullptr)) {
        std::cerr << "eglInitialize failed..." << std::endl;
        return -1;
    }
    auto egl_window = safe_ptr(wl_egl_window_create(surface.get(),
                                                    resolution_vec[0],
                                                    resolution_vec[1]));
    if (!egl_window) {
        std::cerr << "wl_egl_window_create failed..." << std::endl;
        return -1;
    }
    auto configure = [&](int width, int height) noexcept {
        if (width * height) {
            wl_egl_window_resize(egl_window.get(), width, height, 0, 0);
            glViewport(0, 0, width, height);
            resolution_vec[0] = width;
            resolution_vec[1] = height;
        }
    };
    zxdg_toplevel_v6_listener toplevel_listener = {
        .configure = [](auto data, auto, int width, int height, auto) noexcept {
            (*reinterpret_cast<decltype (configure)*>(data))(width, height);
        },
        .close = [](auto...) noexcept { },
    };
    if (zxdg_toplevel_v6_add_listener(toplevel.get(), &toplevel_listener, &configure) != 0) {
        std::cerr << "zxdg_toplevel_v6_add_listener failed..." << std::endl;
        return -1;
    }
    wl_surface_commit(surface.get());

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        std::cerr << "eglBindAPI failed..." << std::endl;
        return -1;
    }
    EGLConfig config;
    EGLint num_config;
    if (!eglChooseConfig(egl_display.get(),
                         std::array<EGLint, 15>(
                             {
                                 EGL_LEVEL, 0,
                                 EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                 EGL_RED_SIZE, 8,
                                 EGL_GREEN_SIZE, 8,
                                 EGL_BLUE_SIZE, 8,
                                 EGL_ALPHA_SIZE, 8,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                 EGL_NONE,
                             }
                         ).data(),
                         &config, 1, &num_config))
    {
        std::cerr << "eglChooseConfig failed..." << std::endl;
        return -1;
    }
    auto egl_context = safe_ptr(eglCreateContext(egl_display.get(),
                                                 config,
                                                 EGL_NO_CONTEXT,
                                                 std::array<EGLint, 3>(
                                                     {
                                                         EGL_CONTEXT_CLIENT_VERSION, 2,
                                                         EGL_NONE,
                                                     }
                                                 ).data()),
                                [&egl_display](auto ptr) noexcept {
                                    eglDestroyContext(egl_display.get(), ptr);
                                });
    if (!egl_context) {
        std::cerr << "eglCreateContext failed..." << std::endl;
        return -1;
    }
    auto egl_surface = safe_ptr(eglCreateWindowSurface(egl_display.get(),
                                                       config,
                                                       egl_window.get(),
                                                       nullptr),
                                [&egl_display](auto ptr) noexcept {
                                    eglDestroySurface(egl_display.get(), ptr);
                                });
    if (!egl_surface) {
        std::cerr << "eglCreateWindowSurface failed..." << std::endl;
        return -1;
    }
    if (!eglMakeCurrent(egl_display.get(),
                        egl_surface.get(), egl_surface.get(),
                        egl_context.get()))
    {
        std::cerr << "eglMakeCurrent failed..." << std::endl;
        return -1;
    }
    auto keyboard = safe_ptr(wl_seat_get_keyboard(seat.get()));
    if (!keyboard) {
        std::cerr << "wl_seat_get_keyboard failed..." << std::endl;
        return -1;
    }
    int key = 0;
    int state = 0;
    auto keyboard_key = [&](uint32_t k, uint32_t s) noexcept {
        key = k;
        state = s;
    };
    wl_keyboard_listener keyboard_listener = {
        .keymap = [](auto...) noexcept { },
        .enter = [](auto...) noexcept { },
        .leave = [](auto...) noexcept { },
        .key = [](auto data, auto, auto, auto, uint32_t key, uint32_t state) {
            (*reinterpret_cast<decltype (keyboard_key)*>(data))(key, state);
        },
        .modifiers = [](auto...) noexcept { },
        .repeat_info = [](auto...) noexcept { },
    };
    if (wl_keyboard_add_listener(keyboard.get(), &keyboard_listener, &keyboard_key) != 0) {
        std::cerr << "wl_keyboard_add_listener failed..." << std::endl;
        return -1;
    }
    auto pointer = safe_ptr(wl_seat_get_pointer(seat.get()));
    if (!pointer) {
        std::cerr << "wl_seat_get_pointer failed..." << std::endl;
        return -1;
    }
    float pointer_vec[2];
    auto pointer_motion = [&](wl_fixed_t x, wl_fixed_t y) noexcept {
        pointer_vec[0] = static_cast<float>(wl_fixed_to_int(x));
        pointer_vec[1] = resolution_vec[1] - static_cast<float>(wl_fixed_to_int(y));
    };
    wl_pointer_listener pointer_listener = {
        .enter = [](auto...) noexcept { },
        .leave = [](auto...) noexcept { },
        .motion = [](void* data, auto, auto, auto x, auto y) noexcept {
            (*reinterpret_cast<decltype (pointer_motion)*>(data))(x, y);
        },
        .button = [](auto...) noexcept { },
        .axis = [](auto...) noexcept { },
        .frame = [](auto...) noexcept { },
        .axis_source = [](auto...) noexcept { },
        .axis_stop = [](auto...) noexcept { },
        .axis_discrete = [](auto...) noexcept { },
    };
    if (wl_pointer_add_listener(pointer.get(), &pointer_listener, &pointer_motion) != 0) {
        std::cerr << "wl_pointer_add_listener failed..." << std::endl;
        return -1;
    }

    if (wl_display_roundtrip(display.get()) == -1) {
        std::cerr << "wl_display_roundtrip failed..." << std::endl;
        return -1;
    }
    auto program = glCreateProgram();
    auto compile = [program](int shader, char const* code) noexcept {
        int compiled = 0;
        if (auto id = glCreateShader(shader)) {
            glShaderSource(id, 1, &code, nullptr);
            glCompileShader(id);
            glGetShaderiv(id, GL_COMPILE_STATUS, &compiled);
            if (compiled) {
                glAttachShader(program, id);
            }
            glDeleteShader(id);
        }
        return compiled;
    };
#define CODE(x) (#x)
    if (!compile(GL_VERTEX_SHADER,
                 CODE(attribute vec4 position;
                      varying vec2 vert;
                      void main(void) {
                          vert = position.xy;
                          gl_Position = position;
                      })))
    {
        std::cerr << "vertex shader compilation failed..." << std::endl;
        return -1;
    }
    if (!compile(GL_FRAGMENT_SHADER,
                 CODE(precision mediump float;
                      varying vec2 vert;
                      uniform vec2 resolution;
                      uniform vec2 pointer;
                      void main(void) {
                          float brightness = length(gl_FragCoord.xy - resolution / 2.0);
                          brightness /= length(resolution);
                          brightness = 1.0 - brightness;
                          gl_FragColor = vec4(0.0, 0.0, brightness, brightness);
                          float radius = length(pointer - gl_FragCoord.xy);
                          float touchMark = smoothstep(16.0, 40.0, radius);
                          gl_FragColor *= touchMark;
                      })))
    {
        std::cerr << "fragment shader compilation failed..." << std::endl;
        return -1;
    }
#undef CODE
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        std::cerr << "linker failed..." << std::endl;
        return -1;
    }
    glUseProgram(program);
    glFrontFace(GL_CW);
    do {
        if (key == 1 && state == 0) {
            return 0;
        }
        glClearColor(0.0, 0.8, 0.0, 0.8);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(program);
        glUniform2fv(glGetUniformLocation(program, "resolution"), 1, resolution_vec);
        glUniform2fv(glGetUniformLocation(program, "pointer"), 1, pointer_vec);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0,
                              std::array<float, 12>({
                                      -1, +1, 0,
                                      +1, +1, 0,
                                      +1, -1, 0,
                                      -1, -1, 0,
                                  }).data());
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        eglSwapBuffers(egl_display.get(), egl_surface.get());
    } while (wl_display_dispatch(display.get()) != -1);
    return 0;
}
