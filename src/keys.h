#pragma once
#include <windows.h>
#include <string>
#include <vector>

// A game key binding such as "Ctrl+C", as DirectInput scan codes (the game reads
// the keyboard through DirectInput: its bindings are DIK codes).
struct KeyCombo {
    std::vector<BYTE> keys; // modifiers first, in press order
    bool recenter = false;  // "Recenter": not a key, recenters the VR view
    bool Empty() const { return keys.empty() && !recenter; }
};

// Parses "N", "Shift+N", "Ctrl+C", "F5", "0x31", "Recenter". Empty text gives an empty combo.
bool ParseKeyCombo(const std::wstring& text, KeyCombo& out);

namespace GameKeys {

// Presses the combo now and releases it a few frames later (the game polls once per frame).
void Press(const KeyCombo& combo);
// Once per frame: releases keys that have been held long enough.
void Update();

} // namespace GameKeys
