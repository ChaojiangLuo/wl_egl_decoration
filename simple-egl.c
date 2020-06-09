/*
 * Copyright © 2011 Benjamin Franzke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <signal.h>

#include <linux/input.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <sys/types.h>
#include <unistd.h>

#include <libdecoration/libdecoration.h>
#include <stdio.h>

#ifndef MIN
#define MIN(x,y) (((x) < (y)) ? (x) : (y))
#endif

struct window;
struct seat;

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct {
		EGLDisplay dpy;
		EGLContext ctx;
		EGLConfig conf;
	} egl;
	struct window *window;
};

struct geometry {
	int width, height;
};

struct window {
	struct display *display;
	struct geometry geometry, window_size;

	uint32_t benchmark_time, frames;
	struct wl_egl_window *native;
	struct wl_surface *surface;
	struct wl_surface *focus;
	EGLSurface egl_surface;
	bool wait_for_configure;
	struct libdecor_frame *frame;
};

static int running = 1;

static void
init_egl(struct display *display, struct window *window)
{
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint major, minor, n, count;
	EGLConfig *configs;
	EGLBoolean ret;

	display->egl.dpy = eglGetDisplay((EGLNativeDisplayType) display->display);
	assert(display->egl.dpy);

	ret = eglInitialize(display->egl.dpy, &major, &minor);
	assert(ret == EGL_TRUE);
	ret = eglBindAPI(EGL_OPENGL_ES_API);
	assert(ret == EGL_TRUE);

	if (!eglGetConfigs(display->egl.dpy, NULL, 0, &count) || count < 1)
		assert(0);

	configs = calloc(count, sizeof *configs);
	assert(configs);

	ret = eglChooseConfig(display->egl.dpy, config_attribs,
			      configs, count, &n);
	assert(ret && n >= 1);

	display->egl.conf = configs[0];

	display->egl.ctx = eglCreateContext(display->egl.dpy,
					    display->egl.conf,
					    EGL_NO_CONTEXT, context_attribs);

	free(configs);

	assert(display->egl.ctx);
}

static void
fini_egl(struct display *display)
{
	eglTerminate(display->egl.dpy);
	eglReleaseThread();
}

static void
create_surface(struct window *window)
{
	struct display *display = window->display;
	EGLBoolean ret;

	window->surface = wl_compositor_create_surface(display->compositor);

	window->native =
		wl_egl_window_create(window->surface,
				     window->geometry.width,
				     window->geometry.height);
	window->egl_surface =
		eglCreateWindowSurface(display->egl.dpy, display->egl.conf,
				      (EGLNativeWindowType) window->native, NULL);

	window->wait_for_configure = true;
	wl_surface_commit(window->surface);

	ret = eglMakeCurrent(window->display->egl.dpy, window->egl_surface,
			     window->egl_surface, window->display->egl.ctx);
	assert(ret == EGL_TRUE);
}

static void
destroy_surface(struct window *window)
{
	/* Required, otherwise segfault in egl_dri2.c: dri2_make_current()
	 * on eglReleaseThread(). */
	eglMakeCurrent(window->display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	eglDestroySurface(window->display->egl.dpy, window->egl_surface);
	wl_egl_window_destroy(window->native);

	wl_surface_destroy(window->surface);
}

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	struct display *display = window->display;
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	eglSwapBuffers(display->egl.dpy, window->egl_surface);
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry, name,
					 &wl_compositor_interface,
					 MIN(version, 4));
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static void
signal_int(int signum)
{
	running = 0;
}

static void
frame_configure(struct libdecor_frame *frame,
		struct libdecor_configuration *configuration,
		void *user_data)
{
	struct window *window = user_data;
	int width, height;
	struct libdecor_state *state;

	window->wait_for_configure = false;

	if (!libdecor_configuration_get_content_size(configuration, frame,
						     &width, &height)) {
		width = window->geometry.width;
		height = window->geometry.height;
	}

	if (width > 0 && height > 0) {
		window->geometry.width = width;
		window->geometry.height = height;
	}

	if (window->native)
		wl_egl_window_resize(window->native,
				     window->geometry.width,
				     window->geometry.height, 0, 0);

	state = libdecor_state_new(width, height);
	libdecor_frame_commit(frame, state, configuration);
	libdecor_state_free(state);
}

static void
frame_close(struct libdecor_frame *frame,
	    void *user_data)
{
	running = 0;
}

static void
frame_commit(void *user_data)
{
	struct window *window = user_data;

	eglSwapBuffers(window->display->display, window->egl_surface);
}

static struct libdecor_frame_interface libdecor_frame_iface = {
	frame_configure,
	frame_close,
	frame_commit,
};

int
main(int argc, char **argv)
{
	struct sigaction sigint;
	struct display display = { 0 };
	struct window  window  = { 0 };
	int ret = 0;
	struct libdecor *context;

	window.display = &display;
	display.window = &window;
	window.geometry.width  = 250;
	window.geometry.height = 250;
	window.window_size = window.geometry;

	display.display = wl_display_connect(NULL);
	assert(display.display);

	display.registry = wl_display_get_registry(display.display);
	wl_registry_add_listener(display.registry,
				 &registry_listener, &display);

	wl_display_roundtrip(display.display);

	init_egl(&display, &window);
	create_surface(&window);

	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	context = libdecor_new(display.display, NULL);

	window.frame = libdecor_decorate(context, window.surface, &libdecor_frame_iface, &window);
	if (!window.frame)
		exit(EXIT_FAILURE);

	libdecor_frame_set_app_id(window.frame, "EGL decoration demo");
	libdecor_frame_set_title(window.frame, "EGL decoration demo");
	libdecor_frame_map(window.frame);

	/* The mainloop here is a little subtle.  Redrawing will cause
	 * EGL to read events so we can just call
	 * wl_display_dispatch_pending() to handle any events that got
	 * queued up as a side effect. */
	while (running && ret != -1) {
		if (window.wait_for_configure) {
			ret = wl_display_dispatch(display.display);
		} else {
			ret = wl_display_dispatch_pending(display.display);
			redraw(&window, NULL, 0);
		}
	}

	fprintf(stderr, "simple-egl exiting\n");

	libdecor_frame_unref(window.frame);

	libdecor_unref(context);

	destroy_surface(&window);
	fini_egl(&display);

	if (display.compositor)
		wl_compositor_destroy(display.compositor);

	wl_registry_destroy(display.registry);
	wl_display_flush(display.display);
	wl_display_disconnect(display.display);

	return 0;
}
