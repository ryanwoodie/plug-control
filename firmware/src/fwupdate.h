// Firmware update from a phone on the controller's Wi-Fi: http://192.168.4.1/update
// takes a firmware.bin plus the update PIN (UPDATE_PIN in secrets.h).
//
// A new firmware stays "on trial" until it has run for 2 min with the cell
// link up (or 10 min without it). If it crashes or hangs before that, the
// bootloader goes back to the previous one by itself. The page can also
// switch back to the previous firmware by hand.
#pragma once
#include <WebServer.h>

void updateSetup(WebServer &web, const char *version);
void updateLoop();
