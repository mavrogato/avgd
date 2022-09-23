
#include <iostream>
#include <memory>
#include <stdexcept>

#include <CL/sycl.hpp>

#include <wayland-client.h>
#include <wayland-egl.h> // must be included before EGL/egl.h
#include "xdg-shell-client.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

template <class T>
auto safe_ptr(T* ptr, auto deleter) noexcept {
    return std::unique_ptr<T, decltype (deleter)>(ptr, deleter);
}
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

#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)
#define TRY(expr) [](auto evaluated) {                                  \
        if (!evaluated) {                                               \
            throw std::runtime_error(__FILE__ ":" TO_STRING(__LINE__) ":"  #expr); \
        }                                                               \
        return evaluated;                                               \
    } (expr)

int main() {
    try {
        auto display = safe_ptr(TRY(wl_display_connect(nullptr)));
        auto registry = safe_ptr(TRY(wl_display_get_registry(display.get())));
        void* compositor_raw = nullptr;
        void* shell_raw = nullptr;
        void* seat_raw = nullptr;
        auto registry_global = [&](auto name, std::string_view interface, auto version) noexcept {
            if (interface == wl_compositor_interface.name) {
                compositor_raw = wl_registry_bind(registry.get(),
                                                  name,
                                                  &wl_compositor_interface,
                                                  version);
            }
            else if (interface == zxdg_shell_v6_interface.name) {
                shell_raw = wl_registry_bind(registry.get(),
                                             name,
                                             &zxdg_shell_v6_interface,
                                             version);
            }
            else if (interface == wl_seat_interface.name) {
                seat_raw = wl_registry_bind(registry.get(),
                                            name,
                                            &wl_seat_interface,
                                            version);
            }
        };
        wl_registry_listener registry_listener {
            .global = [](auto data, auto, auto... args) noexcept {
                (*reinterpret_cast<decltype (registry_global)*>(data))(args...);
            },
            .global_remove = [](auto...) noexcept { },
        };
        TRY(0 == wl_registry_add_listener(registry.get(), &registry_listener, &registry_global));
        TRY(-1 != wl_display_roundtrip(display.get()));
        auto compositor = safe_ptr(reinterpret_cast<wl_compositor*>(TRY(compositor_raw)));
        auto shell = safe_ptr(reinterpret_cast<zxdg_shell_v6*>(TRY(shell_raw)));
        auto seat = safe_ptr(reinterpret_cast<wl_seat*>(TRY(seat_raw)));
        zxdg_shell_v6_listener shell_listener {
            .ping = [](auto, auto shell, auto serial) noexcept {
                zxdg_shell_v6_pong(shell, serial);
            },
        };
        TRY(0 == zxdg_shell_v6_add_listener(shell.get(), &shell_listener, nullptr));
        auto surface = safe_ptr(TRY(wl_compositor_create_surface(compositor.get())));
        auto xdg_surface = safe_ptr(TRY(zxdg_shell_v6_get_xdg_surface(shell.get(),
                                                                      surface.get())));
        zxdg_surface_v6_listener xdg_surface_listener {
            .configure = [](auto, auto xdg_surface, auto serial) noexcept {
                zxdg_surface_v6_ack_configure(xdg_surface, serial);
            }
        };
        TRY(0 == zxdg_surface_v6_add_listener(xdg_surface.get(), &xdg_surface_listener, nullptr));
        auto toplevel = safe_ptr(TRY(zxdg_surface_v6_get_toplevel(xdg_surface.get())));
        auto egl_display = safe_ptr(TRY(eglGetDisplay(display.get())), eglTerminate);
        TRY(eglInitialize(egl_display.get(), nullptr, nullptr));
        float resolution_vec[2] = { 640, 480 };
        auto egl_window = safe_ptr(TRY(wl_egl_window_create(surface.get(),
                                                            resolution_vec[0],
                                                            resolution_vec[1])));
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
        TRY(0 == zxdg_toplevel_v6_add_listener(toplevel.get(), &toplevel_listener, &configure));
        wl_surface_commit(surface.get()); // <- buffer committed to unconfigured xdg_surface
        TRY(eglBindAPI(EGL_OPENGL_ES_API));
        EGLConfig config;
        EGLint num_config;
        TRY(eglChooseConfig(egl_display.get(),
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
                            &config, 1, &num_config));
        auto egl_context = safe_ptr(TRY(eglCreateContext(egl_display.get(),
                                                         config,
                                                         EGL_NO_CONTEXT,
                                                         std::array<EGLint, 3>(
                                                             {
                                                                 EGL_CONTEXT_CLIENT_VERSION, 2,
                                                                 EGL_NONE,
                                                             }
                                                         ).data())),
                                    [&egl_display](auto ptr) noexcept {
                                        eglDestroyContext(egl_display.get(), ptr);
                                    });
        auto egl_surface = safe_ptr(TRY(eglCreateWindowSurface(egl_display.get(),
                                                               config,
                                                               egl_window.get(),
                                                               nullptr)),
                                    [&egl_display](auto ptr) noexcept {
                                        eglDestroySurface(egl_display.get(), ptr);
                                    });
        TRY(eglMakeCurrent(egl_display.get(),
                           egl_surface.get(), egl_surface.get(),
                           egl_context.get()));
        auto keyboard = safe_ptr(TRY(wl_seat_get_keyboard(seat.get())));
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
        TRY(0 == wl_keyboard_add_listener(keyboard.get(), &keyboard_listener, &keyboard_key));
        auto pointer = safe_ptr(TRY(wl_seat_get_pointer(seat.get())));
        float pointer_vec[2] = { 320, 240 };
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
        TRY(0 == wl_pointer_add_listener(pointer.get(), &pointer_listener, &pointer_motion));
        auto touch = safe_ptr(TRY(wl_seat_get_touch(seat.get())));
        wl_touch_listener touch_listener = {
            .down = [](auto...) noexcept { },
            .up = [](auto...) noexcept { },
            .motion = [](void* data, auto, auto, auto, auto x, auto y) noexcept {
                (*reinterpret_cast<decltype (pointer_motion)*>(data))(x, y);
            },
            .frame = [](auto...) noexcept { },
            .cancel = [](auto...) noexcept { },
            .shape = [](auto...) noexcept { },
            .orientation = [](auto...) noexcept { },
        };
        TRY(0 == wl_touch_add_listener(touch.get(), &touch_listener, &pointer_motion));
        auto program = TRY(glCreateProgram());
        auto compile = [](auto program, auto shader_type, char const* code) noexcept {
            GLint compiled = 0;
            if (auto id = glCreateShader(shader_type)) {
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
        TRY(compile(program,
                    GL_VERTEX_SHADER,
                    TO_STRING(attribute vec4 position;
                              varying vec2 vert;
                              void main(void) {
                                  vert = position.xy;
                                  gl_Position = position;
                              })));
        TRY(compile(program,
                    GL_FRAGMENT_SHADER,
                    TO_STRING(precision mediump float;
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
                              })));
        glBindAttribLocation(program, 0, "position");
        auto link = [](auto program) noexcept {
            glLinkProgram(program);
            GLint linked;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            return linked;
        };
        TRY(link(program));
        glUseProgram(program);
        glFrontFace(GL_CW);
        do {
            if (key == 1 && state == 0) {
                return 0;
            }
            glClearColor(0.0, 0.0, 0.8, 0.8);
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
    catch (std::exception& ex) {
        std::cerr << ex.what() << std::endl;
    }
    return -1;
}

