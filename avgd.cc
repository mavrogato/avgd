
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
                sycl::bit_cast<wl_compositor*>(wl_registry_bind(registry.get(),
                                                                name,
                                                                &wl_compositor_interface,
                                                                version)));
        }
        else if (interface == zxdg_shell_v6_interface.name) {
            shell.reset(sycl::bit_cast<zxdg_shell_v6*>(wl_registry_bind(registry.get(),
                                                                        name,
                                                                        &zxdg_shell_v6_interface,
                                                                        version)));
        }
        else if (interface == wl_seat_interface.name) {
            seat.reset(sycl::bit_cast<wl_seat*>(wl_registry_bind(registry.get(),
                                                                 name,
                                                                 &wl_seat_interface,
                                                                 version)));
        }
    };
    wl_registry_listener registry_listener {
        .global = [](auto data, auto, auto... args) noexcept {
            (*sycl::bit_cast<decltype (registry_global)*>(data))(args...);
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
    std::complex<double> resolution(640, 480);
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
                                                    resolution.real(),
                                                    resolution.imag()));
    if (!egl_window) {
        std::cerr << "wl_egl_window_create failed..." << std::endl;
        return -1;
    }
    auto toplevel_configure = [&](int width, int height) noexcept {
        if (width * height) {
            wl_egl_window_resize(egl_window.get(), width, height, 0, 0);
            glViewport(0, 0, width, height);
            resolution = std::complex<double>(width, height);
        }
    };
    zxdg_toplevel_v6_listener toplevel_listener = {
        .configure = [](auto data, auto, int width, int height, auto) noexcept {
            (*sycl::bit_cast<decltype (toplevel_configure)*>(data))(width, height);
        },
        .close = [](auto...) noexcept { },
    };
    if (zxdg_toplevel_v6_add_listener(toplevel.get(), &toplevel_listener, &toplevel_configure) != 0) {
        std::cerr << "zxdg_toplevel_v6_add_listener failed..." << std::endl;
        return -1;
    }
    if (wl_display_roundtrip(display.get()) == -1) {
        std::cerr << "wl_display_roundtrip failed..." << std::endl;
        return -1;
    }
    do {
        wl_surface_commit(surface.get());
        wl_display_flush(display.get());
    } while (wl_display_dispatch(display.get()) != -1);
    return 0;
}
