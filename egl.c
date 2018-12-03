// $ gcc -lEGL -lGLESv2 -lwayland-client -lwayland-egl egl.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define WIDTH 500
#define HEIGHT 400

struct wl_display *display;
struct wl_compositor *compositor;
struct wl_surface *surface;
struct wl_egl_window *egl_window;
struct wl_shell *shell;
struct wl_shell_surface *shell_surface;

EGLDisplay egl_display;
EGLConfig egl_conf;
EGLSurface egl_surface;
EGLContext egl_context;

void *shm_data;

void init_egl() {
  EGLint major, minor, count, n, size;
  EGLConfig *configs;
  int i;
  EGLint config_attribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8, 
    EGL_BLUE_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
  };

  static const EGLint context_attribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };

  egl_display = eglGetDisplay((EGLNativeDisplayType) display);
  if (egl_display == EGL_NO_DISPLAY) {
    fprintf(stderr, "Could not create egl display\n");
    exit(1);
  } else {
    fprintf(stderr, "Created egl display\n");
  }

  if (eglInitialize(egl_display, &major, &minor) != EGL_TRUE) {
    fprintf(stderr, "Could not initialize egl display\n");
    exit(1);
  }
  printf("EGL major/minor: %d/%d\n", major, minor);

  eglGetConfigs(egl_display, NULL, 0, &count);
  printf("EGL has %d configs\n", count);

  configs = (EGLConfig *) malloc(count * sizeof(EGLConfig));
  eglChooseConfig(egl_display, config_attribs, configs, count, &n);

  for (i = 0; i < n; i++) {
    eglGetConfigAttrib(egl_display, configs[i], EGL_BUFFER_SIZE, &size);
    printf("config[%d] buffer size: %d", i, size);
    eglGetConfigAttrib(egl_display, configs[i], EGL_RED_SIZE, &size);
    printf("config[%d] red size: %d", i, size);
  }

  egl_conf = configs[0];
  egl_context = eglCreateContext(egl_display, egl_conf, EGL_NO_CONTEXT, context_attribs);
}

int pixel_value = 0x0;

void paint_pixels() {
  int n;
  uint32_t *pixel = shm_data;

  for (n = 0; n < WIDTH * HEIGHT; n++) {
    pixel[n] = pixel_value; // black
  }

  pixel_value += 0x010101;
  if (pixel_value > 0xffffff) {
    pixel_value = 0x0;
  }
}

static const struct wl_callback_listener frame_listener;

void create_window() {
  egl_window = wl_egl_window_create(surface, WIDTH, HEIGHT);
  if (egl_window == EGL_NO_SURFACE) {
    fprintf(stderr, "Could not create egl window\n");
    exit(1);
  } else {
    fprintf(stderr, "Created egl window\n");
  }

  egl_surface = eglCreateWindowSurface(egl_display, egl_conf, egl_window, NULL);
  if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
    fprintf(stderr, "Made current\n");
  } else {
    fprintf(stderr, "Made current error\n");
  }

  glClearColor(1.0, 1.0, 0.0, 0.4);
  glClear(GL_COLOR_BUFFER_BIT);
  glFlush();

  if (eglSwapBuffers(egl_display, egl_surface)) {
    fprintf(stderr, "Swapped buffers\n");
  } else {
    fprintf(stderr, "Swapping buffers error\n");
  }
}

void create_opaque_region() {
  struct wl_region *region = wl_compositor_create_region(compositor);
  wl_region_add(region, 0, 0, WIDTH, HEIGHT);
  wl_surface_set_opaque_region(surface, region);
}

void global_add(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
  printf("Added: %s, %d\n", interface, id);
  if (strcmp(interface, "wl_compositor") == 0) {
    compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
  }

  if (strcmp(interface, "wl_shell") == 0) {
    shell = wl_registry_bind(registry, id, &wl_shell_interface, 1);
  }
}

void global_remove(void *data, struct wl_registry *registry, uint32_t id) {
  printf("Removed: %d\n", id);
}

struct wl_registry_listener registry_listener = {
  .global = global_add,
  .global_remove = global_remove
};

int main(int argc, char **argv) {
  display = wl_display_connect(NULL);
  if (display == NULL) {
    perror("Can't connect to the display\n");
    exit(1);
  }
  printf("connected to the display\n");

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  
  wl_display_dispatch(display);
  wl_display_roundtrip(display); // wait

  if (compositor == NULL) {
    perror("Could not find any compositor\n");
    exit(1);
  } else {
    printf("Found compositor!\n");
  }

  if (shell == NULL) {
    perror("Could not find any shell\n");
    exit(1);
  } else {
    printf("Found shell!\n");
  }

  surface = wl_compositor_create_surface(compositor);
  if (surface == NULL) {
    perror("Could not create a surface\n");
    exit(1);
  } else {
    printf("Created a surface\n");
  }

  shell_surface = wl_shell_get_shell_surface(shell, surface);
  if (shell_surface == NULL) {
    perror("Could not create a shell surface\n");
    exit(1);
  } else {
    printf("Created a shell surface\n");
  }
  wl_shell_surface_set_toplevel(shell_surface);
  // wl_shell_surface_add_listener(shell_surface, &shell_surface_listener, NULL);

  create_opaque_region();
  init_egl();
  create_window();

  while (wl_display_dispatch(display) != -1) {
    // do nothing
  }

  wl_display_disconnect(display);
  printf("disconnected from the display\n");

  return 0;
}