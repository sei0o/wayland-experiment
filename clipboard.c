#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <poll.h>
#include <errno.h>
#include <linux/input.h>
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>

unsigned win_width = 400;
unsigned win_height = 400;

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
struct wl_data_device_manager *data_device_man;
struct wl_data_device *data_device;
struct wl_data_offer *data_offer;
struct wl_data_source *data_source;
struct wl_data_offer *drag_offer;
struct wl_data_source *drag_source;

void *shm_data;
char *clipboard, *drag_content;
size_t clipboard_size, drag_content_size;
int clipboard_fd, drag_fd;
char *copy_text;
int epfd;
uint32_t drag_enter_serial;
uint32_t drag_action;

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

  for (n = 0; n < win_width * win_height; n++) {
    pixel[n] = 0xff000000;
  }
}

static const struct wl_callback_listener frame_listener;

uint32_t ht;

void redraw(void *data, struct wl_callback *callback, uint32_t time) {
  wl_callback_destroy(frame_callback);
  wl_surface_damage(surface, 0, 0, win_width, win_height);
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
  int line = win_width * 4; // 4 bytes/px
  int size = line * win_height;
  int fd;
  struct wl_buffer *buf;

  ht = win_height;

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
  buf = wl_shm_pool_create_buffer(pool, 0, win_width, win_height, line, WL_SHM_FORMAT_ARGB8888);
  
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

struct wl_data_source_listener data_source_listener;

void drag(uint32_t serial) {
  char text[] = "way way wayland";
  if (copy_text) free(copy_text);
  copy_text = (char *)malloc(strlen(text));
  memcpy(copy_text, text, strlen(text)); // ignore tailing '\0'

  drag_source = wl_data_device_manager_create_data_source(data_device_man);
  wl_data_source_offer(drag_source, "text/plain;charset=utf-8");
  wl_data_source_offer(drag_source, "UTF8_STRING");
  wl_data_source_add_listener(drag_source, &data_source_listener, NULL);

  wl_data_source_set_actions(drag_source, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE | WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);

  wl_data_device_start_drag(data_device, drag_source, surface, NULL, serial);
}

void copy(const char *text, uint32_t serial) {
  if (copy_text) free(copy_text);
  copy_text = (char *)malloc(strlen(text));
  memcpy(copy_text, text, strlen(text)); // ignore trailing '\0'

  data_source = wl_data_device_manager_create_data_source(data_device_man);
  wl_data_source_offer(data_source, "text/plain;charset=utf-8");
  wl_data_source_offer(data_source, "UTF8_STRING");
  wl_data_source_add_listener(data_source, &data_source_listener, NULL);
  
  wl_data_device_set_selection(data_device, data_source, serial);
}

void paste() {
  int fd[2], nfd;
  struct epoll_event ev;
  
  if (!data_offer) return;

  if (pipe2(fd, __O_CLOEXEC) == -1) return;

  // see: https://eklitzke.org/blocking-io-nonblocking-io-and-epoll
  ev.events = EPOLLIN;
  ev.data.fd = clipboard_fd = fd[0];
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd[0], &ev) == -1) {
    perror("Could not add fd[0]");
    exit(1);
  }

  wl_data_offer_receive(data_offer, "text/plain;charset=utf-8", fd[1]);
  close(fd[1]); // the source client closes fd[1], doesn't it?

  fprintf(stderr, "wait for the source client / the clipboard manager to send the data...\n");
}

void key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  printf("%d was %s\n", key, state == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released");

  if (key == KEY_C && state == WL_KEYBOARD_KEY_STATE_PRESSED) { // copy on keydown
    fprintf(stderr, "Copy\n");
    char m[64];
    snprintf(m, 64, "way way wayland %u", serial);
    copy(m, serial);
  }

  if (key == KEY_V && state == WL_KEYBOARD_KEY_STATE_PRESSED) { // paste on keydown
    paste();
  }
}

void modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
  // printf(
  //   "Mods Depressed: %d\n"
  //   "Mods Latched: %d\n"
  //   "Mods Locked: %d\n", mods_depressed, mods_latched, mods_locked);
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
}

void pointer_leave(void *data, struct wl_pointer *ptr, uint32_t serial, struct wl_surface *sfc) {
}

void motion(void *data, struct wl_pointer *ptr, uint32_t time, wl_fixed_t sfc_x, wl_fixed_t sfc_y) {
}

void button(void *data, struct wl_pointer *ptr, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
  if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
    drag(serial);
  }

  if (button == BTN_RIGHT) {
    exit(0);
  }
}

void axis(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis, wl_fixed_t value) {
  printf("Axis: %d %d\n", axis, value);
}

void frame(void *data, struct wl_pointer *ptr) {
  // printf("-- Frame Ended --\n");
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

void data_offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type) {
  fprintf(stderr, "[data_offer(%p).offer] MIME type: %s\n", offer, mime_type);
}

void data_offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t source_actions) {
  fprintf(stderr, "[data_offer(%p).source_actions] %u\n", offer, source_actions);
}

void data_offer_action(void *data, struct wl_data_offer *offer, uint32_t action) {
  drag_action = action;
  switch (action) {
    case WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE:
      fprintf(stderr, "[data_offer.action] none\n");
      break;
    case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY:
      fprintf(stderr, "[data_offer.action] copy\n");
      break;
    case WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE:
      fprintf(stderr, "[data_offer.action] move\n");
      break;
    case WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK:
      fprintf(stderr, "[data_offer.action] ask\n");
      break;
    default:
      fprintf(stderr, "[data_offer.action] unreachable\n");
  }
}

struct wl_data_offer_listener data_offer_listener = {
  .offer = data_offer_offer,
  .source_actions = data_offer_source_actions,
  .action = data_offer_action
};

void data_source_target(void *data, struct wl_data_source *dsrc, const char *mime_type) {
  fprintf(stderr, "[data_source.target] MIME type: %s\n", mime_type);
  // ignore types as we only use text/plain;charset=utf-8
}

// the procedure is common between dnds and selections
void data_source_send(void *data, struct wl_data_source *dsrc, const char *mime_type, int32_t fd) {
  fprintf(stderr, "[data_source.send] %s in %s\n", copy_text, mime_type);
  write(fd, copy_text, strlen(copy_text)); // ignore the tailing '\0'
  close(fd);
}

void data_source_cancelled(void *data, struct wl_data_source *old_dsrc) {
  fprintf(stderr, "[data_source.cancelled]\n");
  wl_data_source_destroy(old_dsrc);
}

void data_source_dnd_drop_performed(void *data, struct wl_data_source *dsrc) {

}

void data_source_dnd_finished(void *data, struct wl_data_source *dsrc) {
  wl_data_source_destroy(dsrc);
}

void data_source_action(void *data, struct wl_data_source *dsrc, uint32_t dnd_action) {
  // Since the data source (source client) and the data device (destination client) are same and we have already treated dnd_action in data_offer_action(), do nothing here.
}

struct wl_data_source_listener data_source_listener = {
  .target = data_source_target,
  .send = data_source_send,
  .cancelled = data_source_cancelled,
  .dnd_drop_performed = data_source_dnd_drop_performed,
  .dnd_finished = data_source_dnd_finished,
  .action = data_source_action
};

uint32_t saved_actions;
uint32_t dnd_action_at(wl_fixed_t x, wl_fixed_t y) {
  int sx = x >> 8, sy = y >> 8;

  // left top
  if (sx < 200 && sy < 200) return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
  
  // left bottom
  if (sx < 200 && sy >= 200) return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;

  // right top
  if (sx >= 200 && sy < 200) return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

  // right bottom
  if (sx >= 200 && sy >= 200) return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
}

void data_device_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *id) {
  fprintf(stderr, "[data_device.data_offer] %p\n", id);
  // data_offer = id;
  wl_data_offer_add_listener(id, &data_offer_listener, NULL);
}

void data_device_enter(void *data, struct wl_data_device *dev, uint32_t serial, struct wl_surface *sfc, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer) {
  fprintf(stderr, "[data_device.enter]\n");

  drag_offer = offer;
  drag_enter_serial = serial;
  saved_actions = dnd_action_at(x, y);
  wl_data_offer_set_actions(offer, saved_actions, saved_actions);
  wl_data_offer_accept(drag_offer, drag_enter_serial, "text/plain;charset=utf-8");
}

void data_device_leave(void *data, struct wl_data_device *dev) {
  fprintf(stderr, "[data_device.leave]\n");
  if (drag_offer) wl_data_offer_destroy(drag_offer); 
}

void data_device_motion(void *data, struct wl_data_device *dev, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
  if (saved_actions != dnd_action_at(x, y)) {
    saved_actions = dnd_action_at(x, y);
    wl_data_offer_set_actions(drag_offer, saved_actions, saved_actions);
    wl_data_offer_accept(drag_offer, drag_enter_serial, "text/plain;charset=utf-8");
  }
}

void data_device_drop(void *data, struct wl_data_device *dev) {
  // if the action is "ask", force the "copy" action ;)
  if (drag_action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
    wl_data_offer_set_actions(drag_offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_offer_accept(drag_offer, drag_enter_serial, "text/plain;charset=utf-8");
    return; // shall we call wl_data_offer_finish() here even though we actually didn't complete a DND?
  }

  int fd[2], nfd;
  struct epoll_event ev;
  
  if (!drag_offer) return;
  if (pipe2(fd, __O_CLOEXEC) == -1) return;

  ev.events = EPOLLIN;
  ev.data.fd = drag_fd = fd[0];
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd[0], &ev) == -1) {
    perror("Could not add fd[0]");
    exit(1);
  }

  wl_data_offer_receive(drag_offer, "text/plain;charset=utf-8", fd[1]);
  // wl_display_flush(display);
  close(fd[1]);

  fprintf(stderr, "[data_device.drop] wait for the source client to send the data...\n");

  wl_data_offer_finish(drag_offer);
}

void data_device_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *id) {
  fprintf(stderr, "[data_device.selection] id(new) = %p, data_offer(old) = %p\n", id, data_offer);
  if (data_offer) wl_data_offer_destroy(data_offer);
  data_offer = id; // data_offer can be NULL
}

struct wl_data_device_listener data_device_listener = {
  .data_offer = data_device_data_offer,
  .enter = data_device_enter,
  .leave = data_device_leave,
  .motion = data_device_motion,
  .drop = data_device_drop,
  .selection = data_device_selection
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

  if (strcmp(interface, "wl_data_device_manager") == 0) {
    data_device_man = wl_registry_bind(registry, id, &wl_data_device_manager_interface, 3);
  }

  if (seat != NULL && data_device_man != NULL) {
    data_device = wl_data_device_manager_get_data_device(data_device_man, seat);
    wl_data_device_add_listener(data_device, &data_device_listener, NULL);
  }
}

void global_remove(void *data, struct wl_registry *registry, uint32_t id) {
  printf("Removed: %d\n", id);
}

struct wl_registry_listener registry_listener = {
  .global = global_add,
  .global_remove = global_remove
};

// Shell surface listeners
void handle_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial) {
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

int epoll_read(int fd, char *buf, size_t *sz) {
  int len = read(fd, buf + strlen(buf), 1024 - 1); // -1 is for '\0' we append later

  if (len == 0) { // No more data to paste
    close(fd);
    // We don't have to delete clipboard_fd from epfd manually (see `man epoll`, Q6/A6)
    // if (epoll_ctl(epfd, EPOLL_CTL_DEL, clipboard_fd, &ev) == -1) {
    //   perror("could not delete fd[0]\n");
    //   exit(1);
    // }

    fd = -1;
    buf[strlen(buf)] = '\0'; // append '\0'
    fprintf(stderr, "received: %s\n", buf);

    // reset clipboard
    strcpy(buf, "");
    return 1;
  }

  if (strlen(buf) + 1024 > *sz) {
    *sz += 1024;
    buf = (char *)realloc(buf, *sz);
  }

  return 0;
}

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

  clipboard_size = 1024;
  clipboard = (char *)malloc(clipboard_size);

  drag_content_size = 1024;
  drag_content = (char *)malloc(drag_content_size);

  create_window();
  redraw(NULL, NULL, 0);

  // init epoll
  epfd = epoll_create1(0);
  if (epfd == -1) {
    perror("Could not create epoll instance\n");
    exit(1);
  }

  struct epoll_event disp_ev, clipboard_ev;
  struct epoll_event events[16];
  disp_ev.events = POLLIN;
  disp_ev.data.fd = wl_display_get_fd(display);
  clipboard_ev.events = POLLIN;
  clipboard_ev.data.fd = clipboard_fd = -1;
  epoll_ctl(epfd, EPOLL_CTL_ADD, wl_display_get_fd(display), &disp_ev);
  epoll_ctl(epfd, EPOLL_CTL_ADD, clipboard_fd, &clipboard_ev);

  // dispatch & event loop
  int x;
  while (1) {
    wl_display_flush(display);

    int nfd = epoll_wait(epfd, events, 16, -1);
    if (nfd == -1) {
      perror("epoll_wait error");
      exit(1);
    }

    for (int i = 0; i < nfd; i++) {
      if (events[i].data.fd == wl_display_get_fd(display)) {
        x = wl_display_dispatch(display);
        if (x == -1) break;
      }

      if (events[i].data.fd == clipboard_fd) {
        fprintf(stderr, "[polling] transferring clipboard data...\n");
        if (epoll_read(clipboard_fd, clipboard, &clipboard_size) == 1) break;
      }

      if (events[i].data.fd == drag_fd) {
        fprintf(stderr, "[polling] transferring dnd data...\n");
        if (epoll_read(drag_fd, drag_content, &drag_content_size) == 1) break;
      }
    }
  }

  // cleanup
  wl_seat_release(seat);
  wl_data_offer_destroy(data_offer);
  wl_data_device_destroy(data_device);
  wl_data_device_manager_destroy(data_device_man);
  free(clipboard);
  free(drag_content);

  wl_display_disconnect(display);
  printf("disconnected from the display\n");

  return 0;
}