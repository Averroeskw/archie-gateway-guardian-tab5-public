#pragma once

// Tiny shared flag: the keyboard poll task (app_main) publishes whether the
// snap-on A164 keyboard is currently responding on I2C; UI surfaces (console
// status bar, setup wizard) read it to show a KB indicator. Atomic, safe from
// any task.
void keyboard_status_set(bool connected);
bool keyboard_status_get();
