// $ pacman -S wayland-procotols
// $ gcc -lwayland-client first.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

struct wl_display *display;
struct wl_compositor *compositor;

void global_add(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
  printf("Added: %s, %d\n", interface, id);
  if (strcmp(interface, "wl_compositor") == 0) {
    compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
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

  wl_display_disconnect(display);
  printf("disconnected from the display\n");

  return 0;
}