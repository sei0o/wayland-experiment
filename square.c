#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>

#define WIDTH 500
#define HEIGHT 400

struct wl_display *display;
struct wl_compositor *compositor;
struct wl_surface *surface;
struct wl_shell *shell;
struct wl_shell_surface *shell_surface;
struct wl_shm *shm;

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
    pixel[n = 0xffff;
  }
}

struct wl_buffer *create_buffer() {
  struct wl_shm_pool *pool;
  int line = WIDTH * 4; // 4 bytes/px
  int size = line * HEIGHT;
  int fd;
  struct wl_buffer *buffer;

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
  buffer = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, line, WL_SHM_FORMAT_XRGB8888);

  wl_shm_pool_destroy(pool);
  return buffer;
}

void create_window() {
  struct wl_buffer *buffer = create_buffer();
  wl_surface_attach(surface, buffer, 0, 0);
  wl_surface_commit(surface);
}

void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
  printf("Format %d\n", format);
}

struct wl_shm_listener shm_listener = {
  shm_format
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
  wl_shell_surface_add_listener(shell_surface, &shell_surface_listener, NULL);

  create_window();
  paint_pixels();

  while (wl_display_dispatch(display) != -1) {
    // do nothing
  }

  wl_display_disconnect(display);
  printf("disconnected from the display\n");

  return 0;
}