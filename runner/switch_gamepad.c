/* Raw SDL_Joystick gamepad path for Nintendo Switch homebrew.
 *
 * Button/axis indices follow the Switch HID order used by the portlibs
 * SDL2 driver (devkitPro sdl2-demo/sdl2-simple examples):
 *   buttons: 0 A, 1 B, 2 X, 3 Y, 4 L-Stick, 5 R-Stick, 6 L, 7 R,
 *            8 ZL, 9 ZR, 10 Plus, 11 Minus, 12 Left, 13 Up, 14 Right,
 *            15 Down.
 *   axes: 0 left-X, 1 left-Y, 2 right-X, 3 right-Y.
 */
#ifdef __SWITCH__

#include "switch_gamepad.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

enum {
  kSwitchPadCapacity = 2,
  kSwitchBtnA = 0,
  kSwitchBtnB = 1,
  kSwitchBtnX = 2,
  kSwitchBtnY = 3,
  kSwitchBtnLStick = 4,
  kSwitchBtnRStick = 5,
  kSwitchBtnL = 6,
  kSwitchBtnR = 7,
  kSwitchBtnZL = 8,
  kSwitchBtnZR = 9,
  kSwitchBtnPlus = 10,
  kSwitchBtnMinus = 11,
  kSwitchBtnLeft = 12,
  kSwitchBtnUp = 13,
  kSwitchBtnRight = 14,
  kSwitchBtnDown = 15,
  kSwitchAxisLeftX = 0,
  kSwitchAxisLeftY = 1,
  kSwitchAxisRightX = 2,
  kSwitchAxisRightY = 3,
};

static SDL_Joystick *s_sticks[kSwitchPadCapacity];
static SDL_JoystickID s_stick_ids[kSwitchPadCapacity];

static int FindStick(SDL_JoystickID id) {
  int i;
  for (i = 0; i < kSwitchPadCapacity; i++) {
    if (s_sticks[i] && s_stick_ids[i] == id) return i;
  }
  return -1;
}

static int FindFreeSlot(void) {
  int i;
  for (i = 0; i < kSwitchPadCapacity; i++) {
    if (!s_sticks[i]) return i;
  }
  return -1;
}

int Dkc1SwitchRefreshPads(void) {
  int opened = 0;
  int i, device, count;
  /* Drop detached sticks first so their slots are reusable. */
  for (i = 0; i < kSwitchPadCapacity; i++) {
    if (s_sticks[i] && !SDL_JoystickGetAttached(s_sticks[i])) {
      SDL_JoystickClose(s_sticks[i]);
      s_sticks[i] = NULL;
    }
    if (s_sticks[i]) opened++;
  }
  count = SDL_NumJoysticks();
  for (device = 0; device < count; device++) {
    SDL_JoystickID id;
    SDL_Joystick *stick;
    SDL_JoystickID opened_id;
    int slot;
    if (opened >= kSwitchPadCapacity) break;
    id = SDL_JoystickGetDeviceInstanceID(device);
    if (id >= 0 && FindStick(id) >= 0) continue;
    stick = SDL_JoystickOpen(device);
    if (!stick) continue;
    opened_id = SDL_JoystickInstanceID(stick);
    if (FindStick(opened_id) >= 0) {
      SDL_JoystickClose(stick);
      continue;
    }
    slot = FindFreeSlot();
    if (slot < 0) {
      SDL_JoystickClose(stick);
      break;
    }
    s_sticks[slot] = stick;
    s_stick_ids[slot] = opened_id;
    opened++;
  }
  return opened;
}

void Dkc1SwitchClosePads(void) {
  int i;
  for (i = 0; i < kSwitchPadCapacity; i++) {
    if (s_sticks[i]) SDL_JoystickClose(s_sticks[i]);
    s_sticks[i] = NULL;
  }
}

int Dkc1SwitchPadCount(void) {
  int count = 0;
  int i;
  for (i = 0; i < kSwitchPadCapacity; i++) {
    if (s_sticks[i] && SDL_JoystickGetAttached(s_sticks[i])) count++;
  }
  return count;
}

static uint8_t SwitchTriggerAsAxis(SDL_Joystick *stick, int button) {
  return SDL_JoystickGetButton(stick, button) ? 255 : 0;
}

size_t Dkc1SwitchReadPads(Dkc1GamepadState *out, size_t capacity) {
  size_t reported = 0;
  int i;
  if (!out || capacity == 0) return 0;
  for (i = 0; i < kSwitchPadCapacity && reported < capacity; i++) {
    SDL_Joystick *stick = s_sticks[i];
    Dkc1GamepadState *pad;
    uint32_t buttons = 0;
    int axes;
    Sint16 lx, ly, ry;
    if (!stick || !SDL_JoystickGetAttached(stick)) continue;
    pad = &out[reported++];
    memset(pad, 0, sizeof *pad);
    if (SDL_JoystickGetButton(stick, kSwitchBtnUp)) buttons |= kDkc1GamepadDpadUp;
    if (SDL_JoystickGetButton(stick, kSwitchBtnDown)) buttons |= kDkc1GamepadDpadDown;
    if (SDL_JoystickGetButton(stick, kSwitchBtnLeft)) buttons |= kDkc1GamepadDpadLeft;
    if (SDL_JoystickGetButton(stick, kSwitchBtnRight)) buttons |= kDkc1GamepadDpadRight;
    if (SDL_JoystickGetButton(stick, kSwitchBtnA)) buttons |= kDkc1GamepadA;
    if (SDL_JoystickGetButton(stick, kSwitchBtnB)) buttons |= kDkc1GamepadB;
    if (SDL_JoystickGetButton(stick, kSwitchBtnX)) buttons |= kDkc1GamepadX;
    if (SDL_JoystickGetButton(stick, kSwitchBtnY)) buttons |= kDkc1GamepadY;
    if (SDL_JoystickGetButton(stick, kSwitchBtnPlus)) buttons |= kDkc1GamepadStart;
    if (SDL_JoystickGetButton(stick, kSwitchBtnMinus)) buttons |= kDkc1GamepadBack;
    if (SDL_JoystickGetButton(stick, kSwitchBtnL)) buttons |= kDkc1GamepadLeftShoulder;
    if (SDL_JoystickGetButton(stick, kSwitchBtnR)) buttons |= kDkc1GamepadRightShoulder;
    if (SDL_JoystickGetButton(stick, kSwitchBtnLStick)) buttons |= kDkc1GamepadLeftStick;
    if (SDL_JoystickGetButton(stick, kSwitchBtnRStick)) buttons |= kDkc1GamepadRightStick;
    axes = SDL_JoystickNumAxes(stick);
    lx = axes > kSwitchAxisLeftX ? SDL_JoystickGetAxis(stick, kSwitchAxisLeftX) : 0;
    ly = axes > kSwitchAxisLeftY ? SDL_JoystickGetAxis(stick, kSwitchAxisLeftY) : 0;
    pad->left_x = lx;
    /* Desktop gamepad state uses up-positive Y; SDL joysticks are
     * down-positive. */
    pad->left_y = (ly == INT16_MIN) ? INT16_MAX : (int16_t)-ly;
    pad->right_x = axes > kSwitchAxisRightX ? SDL_JoystickGetAxis(stick, kSwitchAxisRightX) : 0;
    ry = axes > kSwitchAxisRightY ? SDL_JoystickGetAxis(stick, kSwitchAxisRightY) : 0;
    pad->right_y = (ry == INT16_MIN) ? INT16_MAX : (int16_t)-ry;
    /* ZL/ZR are digital on Switch hardware; report them as full-scale
     * trigger axes so the shared L/R trigger bindings fire. */
    pad->left_trigger = SwitchTriggerAsAxis(stick, kSwitchBtnZL);
    pad->right_trigger = SwitchTriggerAsAxis(stick, kSwitchBtnZR);
    /* Left stick doubles as a DPad past the deflection threshold. */
    if (lx < -DKC1_SWITCH_STICK_DPAD_DEADZONE) buttons |= kDkc1GamepadDpadLeft;
    if (lx > DKC1_SWITCH_STICK_DPAD_DEADZONE) buttons |= kDkc1GamepadDpadRight;
    if (ly < -DKC1_SWITCH_STICK_DPAD_DEADZONE) buttons |= kDkc1GamepadDpadUp;
    if (ly > DKC1_SWITCH_STICK_DPAD_DEADZONE) buttons |= kDkc1GamepadDpadDown;
    pad->buttons = buttons;
  }
  return reported;
}

bool Dkc1SwitchOpenControllerApplet(void) {
  HidLaControllerSupportArg arg;
  HidLaControllerSupportResultInfo info;
  Result rc;

  /* Native "connect controllers" screen (grip/order, pairing): requires
   * a 2nd pad for 2-player modes. Blocking and modal; the caller
   * reanchors its clocks on return. */
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = 2;
  arg.hdr.player_count_max = 2;
  arg.enable_explain_text = 1;
  hidLaSetExplainText(&arg, "2P MODE - CONNECT 2ND CONTROLLER",
                      HidNpadIdType_No2);
  rc = hidLaShowControllerSupport(&info, &arg);
  if (R_FAILED(rc)) {
    fprintf(stderr, "[SwitchPad] controller applet failed: 0x%x\n",
            (unsigned)rc);
    return false;
  }
  /* Pairing/grip changes renumber devices: drop everything and rescan
   * so player 2 lands on the fresh stick. */
  Dkc1SwitchClosePads();
  Dkc1SwitchRefreshPads();
  return true;
}

#endif /* __SWITCH__ */
