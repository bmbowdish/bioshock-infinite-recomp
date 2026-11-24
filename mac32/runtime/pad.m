// Claude Fable - 2026-11-24: game controllers, through Apple's GameController framework.
//
// The host side of controller support. It knows nothing about any particular game: it keeps up to four connected
// controllers in numbered slots and hands out their state in the layout Windows games expect (XInput: 16 button
// bits, two 0-255 triggers, four -32768..32767 stick axes). A game project connects this to its engine, usually by
// replacing the game's own "read controller" function (see H_XInputGetState in BioShock Infinite's game32.c).
//
// GameController covers Xbox, PlayStation, Switch Pro and MFi controllers, wired or Bluetooth, with one code path.
// macOS delivers their input on the main thread while the app is in front; each change is copied into a small
// snapshot under a lock, and the game's thread reads the snapshot. Nothing here blocks or polls hardware.
//
// Not done yet: rumble (host_pad_rumble only remembers the request).
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include "game32.h"

#define NPAD 4
// XInput button bits
enum {
    PAD_UP = 0x0001,
    PAD_DOWN = 0x0002,
    PAD_LEFT = 0x0004,
    PAD_RIGHT = 0x0008,
    PAD_START = 0x0010,
    PAD_BACK = 0x0020,
    PAD_LSTICK = 0x0040,
    PAD_RSTICK = 0x0080,
    PAD_LB = 0x0100,
    PAD_RB = 0x0200,
    PAD_A = 0x1000,
    PAD_B = 0x2000,
    PAD_X = 0x4000,
    PAD_Y = 0x8000
};

static os_unfair_lock pad_lock = OS_UNFAIR_LOCK_INIT;
static Rt32Pad pad_snap[NPAD];      // latest state per slot (guarded by pad_lock)
static int pad_present[NPAD];       // slot has a controller (guarded by pad_lock)
static GCController *pad_ctl[NPAD]; // main thread only
static int pad_started;

static int16_t axis16(float v) {
    if (v > 1)
        v = 1;
    if (v < -1)
        v = -1;
    return (int16_t)(v * 32767.0f);
}
static uint8_t trig8(float v) {
    if (v > 1)
        v = 1;
    if (v < 0)
        v = 0;
    return (uint8_t)(v * 255.0f + 0.5f);
}
// Main thread: copy the controller's current values into its slot's snapshot.
static void pad_capture(int slot, GCExtendedGamepad *g) {
    Rt32Pad p = {0};
    uint16_t b = 0;
    if (g.dpad.up.isPressed)
        b |= PAD_UP;
    if (g.dpad.down.isPressed)
        b |= PAD_DOWN;
    if (g.dpad.left.isPressed)
        b |= PAD_LEFT;
    if (g.dpad.right.isPressed)
        b |= PAD_RIGHT;
    if (g.buttonMenu.isPressed)
        b |= PAD_START;
    if (g.buttonOptions && g.buttonOptions.isPressed)
        b |= PAD_BACK;
    if (g.leftThumbstickButton && g.leftThumbstickButton.isPressed)
        b |= PAD_LSTICK;
    if (g.rightThumbstickButton && g.rightThumbstickButton.isPressed)
        b |= PAD_RSTICK;
    if (g.leftShoulder.isPressed)
        b |= PAD_LB;
    if (g.rightShoulder.isPressed)
        b |= PAD_RB;
    if (g.buttonA.isPressed)
        b |= PAD_A;
    if (g.buttonB.isPressed)
        b |= PAD_B;
    if (g.buttonX.isPressed)
        b |= PAD_X;
    if (g.buttonY.isPressed)
        b |= PAD_Y;
    p.buttons = b;
    p.lt = trig8(g.leftTrigger.value);
    p.rt = trig8(g.rightTrigger.value);
    p.lx = axis16(g.leftThumbstick.xAxis.value);
    p.ly = axis16(g.leftThumbstick.yAxis.value);
    p.rx = axis16(g.rightThumbstick.xAxis.value);
    p.ry = axis16(g.rightThumbstick.yAxis.value);
    os_unfair_lock_lock(&pad_lock);
    p.packet = pad_snap[slot].packet + 1; // XInput's "something changed" counter
    pad_snap[slot] = p;
    os_unfair_lock_unlock(&pad_lock);
    if (getenv("RT32_PAD_LOG"))
        fprintf(stderr, "[pad] slot %d: buttons %04x  LT %3u RT %3u  L %6d,%6d  R %6d,%6d\n", slot, p.buttons, p.lt,
                p.rt, p.lx, p.ly, p.rx, p.ry);
}
// Main thread: a controller appeared. Give it the first free slot and follow its changes.
static void pad_attach(GCController *ct) {
    GCExtendedGamepad *g = ct.extendedGamepad;
    if (!g) {
        fprintf(stderr, "[pad] ignored \"%s\": not a full gamepad (no extended profile)\n",
                ct.vendorName.UTF8String ?: "?");
        return;
    }
    int slot = -1;
    for (int i = 0; i < NPAD; i++) {
        if (pad_ctl[i] == ct)
            return; // already attached
        if (slot < 0 && !pad_ctl[i])
            slot = i;
    }
    if (slot < 0) {
        fprintf(stderr, "[pad] no free slot for \"%s\"\n", ct.vendorName.UTF8String ?: "?");
        return;
    }
    pad_ctl[slot] = ct;
    ct.playerIndex = (GCControllerPlayerIndex)slot; // lights the matching player LED where the pad has one
    os_unfair_lock_lock(&pad_lock);
    pad_present[slot] = 1;
    os_unfair_lock_unlock(&pad_lock);
    g.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el) {
      pad_capture(slot, gp);
    };
    pad_capture(slot, g);
    fprintf(stderr, "[pad] controller connected in slot %d: %s (%s)\n", slot, ct.vendorName.UTF8String ?: "?",
            ct.productCategory.UTF8String ?: "?");
}
static void pad_detach(GCController *ct) {
    for (int i = 0; i < NPAD; i++)
        if (pad_ctl[i] == ct) {
            pad_ctl[i] = nil;
            os_unfair_lock_lock(&pad_lock);
            pad_present[i] = 0;
            uint32_t pk = pad_snap[i].packet + 1;
            pad_snap[i] = (Rt32Pad){.packet = pk};
            os_unfair_lock_unlock(&pad_lock);
            fprintf(stderr, "[pad] controller in slot %d disconnected\n", i);
        }
}
// Any thread: start watching for controllers (once). The work itself happens on the main thread.
static void pad_start(void) {
    if (__atomic_exchange_n(&pad_started, 1, __ATOMIC_SEQ_CST))
        return;
    if (getenv("RT32_NO_PAD")) {
        fprintf(stderr, "[pad] controller support switched off (RT32_NO_PAD)\n");
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
      [nc addObserverForName:GCControllerDidConnectNotification
                      object:nil
                       queue:[NSOperationQueue mainQueue]
                  usingBlock:^(NSNotification *n) {
                    pad_attach(n.object);
                  }];
      [nc addObserverForName:GCControllerDidDisconnectNotification
                      object:nil
                       queue:[NSOperationQueue mainQueue]
                  usingBlock:^(NSNotification *n) {
                    pad_detach(n.object);
                  }];
      NSArray<GCController *> *all = [GCController controllers];
      fprintf(stderr, "[pad] watching for controllers (%d connected now)\n", (int)all.count);
      for (GCController *ct in all)
          pad_attach(ct);
    });
}

// Scripted button presses for unattended test runs (RT32_INPUT "frame:pad:0x1000" = press A): the buttons are held
// in slot 0 for the next `reads` polls by the game, on top of whatever a real controller there is doing. Slot 0
// reports "connected" while a scripted press is in progress even with no controller attached.
static uint16_t pad_script_buttons;
static int pad_script_reads;
static uint32_t pad_script_packet;
void host_pad_script(uint16_t buttons, int reads) {
    os_unfair_lock_lock(&pad_lock);
    pad_script_buttons = buttons;
    pad_script_reads = reads;
    pad_script_packet++;
    os_unfair_lock_unlock(&pad_lock);
}

int host_pad_read(int index, Rt32Pad *out) {
    pad_start();
    if (index < 0 || index >= NPAD)
        return 0;
    os_unfair_lock_lock(&pad_lock);
    int here = pad_present[index];
    Rt32Pad p = here ? pad_snap[index] : (Rt32Pad){0};
    if (index == 0 && (pad_script_reads > 0 || pad_script_buttons)) {
        if (pad_script_reads > 0) {
            p.buttons |= pad_script_buttons;
            here = 1;
            if (out && --pad_script_reads == 0) { // released: one more state change
                pad_script_buttons = 0;
                pad_script_packet++;
            }
        }
    }
    p.packet += pad_script_packet;
    if (here && out)
        *out = p;
    os_unfair_lock_unlock(&pad_lock);
    return here;
}
int host_pad_rumble(int index, float low, float high) { // TODO: drive the controller's motors (GCController.haptics)
    pad_start();
    (void)low;
    (void)high;
    return host_pad_read(index, NULL);
}
