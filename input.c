#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <linux/input.h>

#define WIDTH 500
#define HEIGHT 400

struct wl_display *display;
struct wl_compositor *compositor;
struct wl_surface *surface;
struct wl_shell *shell;
struct wl_shell_surface *shell_surface;
struct wl_shm *shm;
struct wl_buffer *buffer;
struct wl_callback *frame_callback;
struct wl_seat *seat;
struct wl_pointer *pointer;
struct wl_keyboard *keyboard;
struct wl_cursor_theme *cursor_theme;
struct wl_cursor *cursor;
struct wl_surface *cursor_sfc;

void *shm_data;

// Shell surface listeners
void handle_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial) {
  wl_shell_surface_pong(shell_surface, serial);
  printf("Pong\n");
}

void handle_configure(void *data, struct wl_shell_surface *shell_surface, uint32_t edges, int32_t width, int32_t height) {

}

void handle_popup_done(void *data, struct wl_shell_surface *shell_surface) {

}

struct wl_shell_surface_listener shell_surface_listener = {
  handle_ping,
  handle_configure,
  handle_popup_done
};

// Dealing with tmpfiles
int set_cloexec_or_close(int fd) {
  long flags;
  if (fd == -1) return -1;
  
  flags = fcntl(fd, F_GETFD);
  if (flags == -1) {
    close(fd);
    return -1;
  }

  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
    close(fd);
    return -1;
  }

  return fd;
}

int create_tmpfile_cloexec(char *tmpname) {
#ifdef HAVE_MKOSTEMP
  int fd = mkostemp(tmpname, O_CLOEXEC);
  if (fd >= 0) unlink(tmpname);
#else
  int fd = mkstemp(tmpname);
  if (fd >= 0) {
    fd = set_cloexec_or_close(fd);
    unlink(tmpname);
  }
#endif

  return fd;
}

int os_create_anonymous_file(off_t size) { // from Weston's implementation
  static const char template[] = "/weston-shared-XXXXXX";
  const char *path;
  char *name;
  int fd;

  path = getenv("XDG_RUNTIME_DIR");
  if (!path) {
    errno = ENOENT;
    return -1;
  }

  name = malloc(strlen(path) + sizeof(template));
  if (!name) return -1;
  strcpy(name, path);
  strcat(name, template); // name += template

  fd = create_tmpfile_cloexec(name);
  free(name);
  if (fd < 0) return -1;
  if (ftruncate(fd, size) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

void paint_pixels() {
  int n;
  uint32_t *pixel = shm_data;

  for (n = 0; n < WIDTH * HEIGHT; n++) {
    pixel[n] = 0xff000000;
  }
}

static const struct wl_callback_listener frame_listener;

uint32_t ht;

void redraw(void *data, struct wl_callback *callback, uint32_t time) {
  wl_callback_destroy(frame_callback);
  wl_surface_damage(surface, 0, 0, WIDTH, HEIGHT);
  paint_pixels();
  frame_callback = wl_surface_frame(surface);
  wl_surface_attach(surface, buffer, 0, 0);
  wl_callback_add_listener(frame_callback, &frame_listener, NULL);
  wl_surface_commit(surface);
}

static const struct wl_callback_listener frame_listener = {
  redraw
};

struct wl_buffer *create_buffer() {
  struct wl_shm_pool *pool;
  int line = WIDTH * 4; // 4 bytes/px
  int size = line * HEIGHT;
  int fd;
  struct wl_buffer *buf;

  ht = HEIGHT;

  fd = os_create_anonymous_file(size);
  if (fd < 0) {
    printf("Failed to create a buffer which has the size of %d\n", size);
    exit(1);
  }

  shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (shm_data == MAP_FAILED) {
    printf("mmap failed: %m\n");
    close(fd);
    exit(1);
  }

  pool = wl_shm_create_pool(shm, fd, size);
  buf = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, line, WL_SHM_FORMAT_ARGB8888);

  wl_shm_pool_destroy(pool);
  return buf;
}

void create_window() {
  buffer = create_buffer();
  wl_surface_attach(surface, buffer, 0, 0);
  wl_surface_commit(surface);
}

void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
  printf("Format %d\n", format);
}

struct wl_shm_listener shm_listener = {
  shm_format
};

void keymap(void *data, struct wl_keyboard *kbd, uint32_t format, int32_t fd, uint32_t size) {

}

void keyboard_enter(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *sfc, struct wl_array *keys) {
  printf("Keyboard entered a surface\n");
}

void keyboard_leave(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *sfc) {
  printf("Keyboard left the surface\n");
}

void key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  printf("%d was %s\n", key, state == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released");
}

void modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
  printf(
    "Mods Depressed: %d\n"
    "Mods Latched: %d\n"
    "Mods Locked: %d\n", mods_depressed, mods_latched, mods_locked);
}

void repeat_info(void *data, struct wl_keyboard *kbd, int32_t rate, int32_t delay) {

}

struct wl_keyboard_listener keyboard_listener = {
  keymap,
  keyboard_enter,
  keyboard_leave,
  key,
  modifiers,
  repeat_info
};

void pointer_enter(void *data, struct wl_pointer *ptr, uint32_t serial, struct wl_surface *sfc, wl_fixed_t sfc_x, wl_fixed_t sfc_y) {
  struct wl_cursor_image *cursor_image = cursor->images[0];
  struct wl_buffer *cursor_buf = wl_cursor_image_get_buffer(cursor_image);
  cursor_sfc = wl_compositor_create_surface(compositor);
  wl_surface_attach(cursor_sfc, cursor_buf, 0, 0);
  wl_surface_damage(cursor_sfc, 0, 0, cursor_image->width, cursor_image->height);
  wl_surface_commit(cursor_sfc);
  wl_pointer_set_cursor(pointer, 0, cursor_sfc, cursor_image->hotspot_x, cursor_image->hotspot_y); // serial?
}

void pointer_leave(void *data, struct wl_pointer *ptr, uint32_t serial, struct wl_surface *sfc) {

}

wl_fixed_t sx, sy;
void motion(void *data, struct wl_pointer *ptr, uint32_t time, wl_fixed_t sfc_x, wl_fixed_t sfc_y) {
  sx = sfc_x;
  sy = sfc_y;
}

void button(void *data, struct wl_pointer *ptr, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
  printf("A button was pushed at (%d, %d)\n", sx, sy);

  if (button == BTN_RIGHT) {
    exit(0);
  }
}

void axis(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis, wl_fixed_t value) {
  printf("Axis: %d %d\n", axis, value);
}

void frame(void *data, struct wl_pointer *ptr) {
  printf("-- Frame Ended --\n");
}

void axis_source(void *data, struct wl_pointer *ptr, uint32_t axis_source) {
  printf("Axis source: %d", axis_source);
}

void axis_stop(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis) {
  printf("Axis stopped: %d", axis);
}

void axis_discrete(void *data, struct wl_pointer *ptr, uint32_t axis, int32_t discrete) {
  printf("Axis discrete: %d %d", axis, discrete);
}

struct wl_pointer_listener pointer_listener = {
  .enter = pointer_enter,
  .leave = pointer_leave,
  .motion = motion,
  .button = button,
  .axis = axis,
  .frame = frame,
  .axis_source = axis_source,
  .axis_stop = axis_stop,
  .axis_discrete = axis_discrete
};

void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
    keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
  }

  if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
    wl_keyboard_release(keyboard);
  }

  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
    pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL);
  }

  if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && pointer) {
    wl_pointer_release(pointer);
  }

  // ignore touchpad etc.
}

void seat_name(void *data, struct wl_seat *seat, const char *name) {
  printf("Seat name: %s", name);
}

struct wl_seat_listener seat_listener = {
  seat_capabilities,
  seat_name
};

void global_add(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
  printf("Added: %s, %d\n", interface, id);
  if (strcmp(interface, "wl_compositor") == 0) {
    compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
  }

  if (strcmp(interface, "wl_shell") == 0) {
    shell = wl_registry_bind(registry, id, &wl_shell_interface, 1);
  }

  if (strcmp(interface, "wl_shm") == 0) {
    shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    wl_shm_add_listener(shm, &shm_listener, NULL);
  }

  if (strcmp(interface, "wl_seat") == 0) {
    seat = wl_registry_bind(registry, id, &wl_seat_interface, 5);
    wl_seat_add_listener(seat, &seat_listener, NULL);
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

  if (seat == NULL) {
    perror("Could not find any seat\n");
    exit(1);
  } else {
    printf("Found seat\n");
  }

  surface = wl_compositor_create_surface(compositor);
  if (surface == NULL) {
    perror("Could not create a surface\n");
    exit(1);
  } else {
    printf("Created a surface\n");
  }

  cursor_theme = wl_cursor_theme_load(NULL, 32, shm);
  cursor = wl_cursor_theme_get_cursor(cursor_theme, "text");
  if (cursor == NULL) {
    perror("Could not get a cursor\n");
    exit(1);
  } else {
    printf("Got a cursor\n");
  }

  shell_surface = wl_shell_get_shell_surface(shell, surface);
  if (shell_surface == NULL) {
    perror("Could not create a shell surface\n");
    exit(1);
  } else {
    printf("Created a shell surface\n");
  }
  wl_shell_surface_set_toplevel(shell_surface);
  wl_shell_surface_add_listener(shell_surface, &shell_surface_listener, NULL);

  frame_callback = wl_surface_frame(surface);
  wl_callback_add_listener(frame_callback, &frame_listener, NULL);

  create_window();
  redraw(NULL, NULL, 0);

  while (wl_display_dispatch(display) != -1) {
    // do nothing
  }

  wl_seat_release(seat);
  wl_cursor_theme_destroy(cursor_theme);
  wl_surface_destroy(cursor_sfc);

  wl_display_disconnect(display);
  printf("disconnected from the display\n");

  return 0;
}