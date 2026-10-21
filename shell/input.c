/* input.c — USB HID + PS/2 keyboard/mouse via kernel evdev, read raw (no libinput).
 *
 * Device-open/scanout patterns adapted from K93OS-x86 k93ui.c (GPL-2.0, permission):
 * keyboard pick by name, open every pointer device (rel deltas summed, abs scaled),
 * US QWERTY scancode tables. Extended: multiple keyboards, EVIOCGRAB, modifier state
 * for Wayland clients, explicit screen size for abs scaling.
 */
#define _GNU_SOURCE
#include "lost.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAXDEV 32

static struct {
  int fd;
  int pointer; /* 0 kbd, 1 mouse/tablet */
  /* abs ranges */
  int abs, minx, maxx, miny, maxy, ax, ay, amoved;
} dev[MAXDEV];
static int ndev;
static int scr_w = 1024, scr_h = 768;
static unsigned mods; /* bit0 shift bit1 caps bit2 ctrl bit3 alt bit4 logo bit5 num */
static int caps_on;

u32 input_key_char(int code, int shift) {
  static const char base[128] = {
      [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5',
      [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0',
      [KEY_MINUS] = '-', [KEY_EQUAL] = '=', [KEY_Q] = 'q', [KEY_W] = 'w',
      [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u',
      [KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p', [KEY_LEFTBRACE] = '[',
      [KEY_RIGHTBRACE] = ']', [KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd',
      [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k',
      [KEY_L] = 'l', [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'',
      [KEY_GRAVE] = '`', [KEY_BACKSLASH] = '\\', [KEY_Z] = 'z', [KEY_X] = 'x',
      [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = 'm',
      [KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/', [KEY_SPACE] = ' ',
  };
  static const char shifted[128] = {
      [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$', [KEY_5] = '%',
      [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*', [KEY_9] = '(', [KEY_0] = ')',
      [KEY_MINUS] = '_', [KEY_EQUAL] = '+', [KEY_Q] = 'Q', [KEY_W] = 'W',
      [KEY_E] = 'E', [KEY_R] = 'R', [KEY_T] = 'T', [KEY_Y] = 'Y', [KEY_U] = 'U',
      [KEY_I] = 'I', [KEY_O] = 'O', [KEY_P] = 'P', [KEY_LEFTBRACE] = '{',
      [KEY_RIGHTBRACE] = '}', [KEY_A] = 'A', [KEY_S] = 'S', [KEY_D] = 'D',
      [KEY_F] = 'F', [KEY_G] = 'G', [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K',
      [KEY_L] = 'L', [KEY_SEMICOLON] = ':', [KEY_APOSTROPHE] = '"',
      [KEY_GRAVE] = '~', [KEY_BACKSLASH] = '|', [KEY_Z] = 'Z', [KEY_X] = 'X',
      [KEY_C] = 'C', [KEY_V] = 'V', [KEY_B] = 'B', [KEY_N] = 'N', [KEY_M] = 'M',
      [KEY_COMMA] = '<', [KEY_DOT] = '>', [KEY_SLASH] = '?', [KEY_SPACE] = ' ',
  };
  if ((unsigned)code >= 128) return 0;
  char c = shift ? shifted[code] : base[code];
  return c ? (u32)c : 0;
}

void input_set_screen(int w, int h) {
  if (w > 0) scr_w = w;
  if (h > 0) scr_h = h;
}

unsigned input_mods(void) { return mods; }

static int classify(int fd) {
  unsigned long rel = 0, absb = 0, keyb = 0;
  int has_rel = ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), &rel) >= 0 && (rel & (1UL << REL_X));
  int has_abs = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), &absb) >= 0 && (absb & (1UL << ABS_X));
  int has_kbd = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), &keyb) >= 0 && (keyb & (1UL << KEY_A));
  if (has_rel || has_abs) return 1;
  if (has_kbd) return 0;
  return -1; /* neither: skip */
}

int input_open(int *fds, int max) {
  /* every event device: keyboards (AT/USB HID) and pointers (PS/2/USB) */
  for (int i = 0; i < MAXDEV && ndev < MAXDEV; i++) {
    char p[64];
    snprintf(p, sizeof p, "/dev/input/event%d", i);
    int fd = open(p, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) continue;
    int kind = classify(fd);
    if (kind < 0) {
      close(fd);
      continue;
    }
    if (kind == 1) {
      struct input_absinfo ai;
      dev[ndev].abs = ioctl(fd, EVIOCGABS(ABS_X), &ai) >= 0 &&
                      ioctl(fd, EVIOCGABS(ABS_Y), &(struct input_absinfo){0}) >= 0;
      if (dev[ndev].abs) {
        dev[ndev].minx = ai.minimum;
        dev[ndev].maxx = ai.maximum > ai.minimum ? ai.maximum : ai.minimum + 1;
        dev[ndev].ax = ai.value;
        if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) >= 0) {
          dev[ndev].miny = ai.minimum;
          dev[ndev].maxy = ai.maximum > ai.minimum ? ai.maximum : ai.minimum + 1;
          dev[ndev].ay = ai.value;
        } else {
          dev[ndev].miny = 0;
          dev[ndev].maxy = 1;
        }
      }
    }
    ioctl(fd, EVIOCGRAB, 1); /* shell is the sole consumer; ignore failure */
    dev[ndev].fd = fd;
    dev[ndev].pointer = kind;
    if (ndev < max) fds[ndev] = fd;
    ndev++;
    if (ndev >= max) break;
  }
  return ndev;
}

int input_is_pointer(int fd) {
  for (int i = 0; i < ndev; i++)
    if (dev[i].fd == fd) return dev[i].pointer;
  return 0;
}

static void mods_key(int code, int pressed) {
  unsigned bit = 0;
  switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: bit = 1u << 0; break;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL: bit = 1u << 2; break;
    case KEY_LEFTALT:
    case KEY_RIGHTALT: bit = 1u << 3; break;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA: bit = 1u << 4; break;
    case KEY_NUMLOCK: bit = 1u << 5; break;
    case KEY_CAPSLOCK:
      if (pressed == 1) caps_on = !caps_on;
      mods = caps_on ? mods | (1u << 1) : mods & ~(1u << 1);
      return;
    default: return;
  }
  if (pressed)
    mods |= bit;
  else
    mods &= ~bit;
}

int input_read(int fd, KeyInput *keys, int *nkeys, int keys_cap, MouseInput *m) {
  int di = -1;
  for (int i = 0; i < ndev; i++)
    if (dev[i].fd == fd) {
      di = i;
      break;
    }
  if (di < 0) return -1;
  struct input_event ie;
  while (read(fd, &ie, sizeof ie) == sizeof ie) {
    if (ie.type == EV_KEY && dev[di].pointer == 0) {
      int val = ie.value; /* 0 up 1 down 2 repeat */
      mods_key(ie.code, val == 0 ? 0 : 1);
      if (*nkeys < keys_cap) {
        keys[*nkeys].code = ie.code;
        keys[*nkeys].pressed = val;
        (*nkeys)++;
      }
    } else if (ie.type == EV_KEY && dev[di].pointer) {
      int b = ie.code == BTN_LEFT ? 0 : ie.code == BTN_RIGHT ? 1
              : ie.code == BTN_MIDDLE ? 2
                                      : -1;
      if (b >= 0) {
        if (ie.value) m->buttons |= 1 << b;
        else m->buttons &= ~(1 << b);
        m->changed = 1;
      }
    } else if (ie.type == EV_REL) {
      if (ie.code == REL_X) {
        m->dx += ie.value;
        m->changed = 1;
      } else if (ie.code == REL_Y) {
        m->dy += ie.value;
        m->changed = 1;
      } else if (ie.code == REL_WHEEL) {
        m->dw += ie.value;
        m->changed = 1;
      }
    } else if (ie.type == EV_ABS && dev[di].abs) {
      if (ie.code == ABS_X)
        dev[di].ax = ie.value;
      else if (ie.code == ABS_Y)
        dev[di].ay = ie.value;
      else
        continue;
      long rx = (long)(dev[di].ax - dev[di].minx) * (scr_w - 1) /
                (long)(dev[di].maxx - dev[di].minx);
      long ry = (long)(dev[di].ay - dev[di].miny) * (scr_h - 1) /
                (long)(dev[di].maxy - dev[di].miny);
      m->abs_x = rx < 0 ? 0 : (rx >= scr_w ? scr_w - 1 : (int)rx);
      m->abs_y = ry < 0 ? 0 : (ry >= scr_h ? scr_h - 1 : (int)ry);
      m->has_abs = 1;
      m->changed = 1;
    }
  }
  return 0;
}
