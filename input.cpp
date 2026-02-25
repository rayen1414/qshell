// ============================================================================
// INPUT ADAPTER  —  input.cpp  v3.0  (Win32 / XInput  —  no raylib)
// ============================================================================

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <cmath>

#pragma comment(lib, "xinput.lib")

// ─── XInput dynamic loader (same pattern as qshell.cpp) ─────────────────────
namespace XInputLocal {
    typedef DWORD(WINAPI* GetStateFn)(DWORD, XINPUT_STATE*);
    static HMODULE    s_lib      = nullptr;
    static GetStateFn s_getState = nullptr;

    static void Load() {
        if (s_lib) return;
        const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (auto d : dlls) {
            s_lib = LoadLibraryA(d);
            if (s_lib) {
                s_getState = reinterpret_cast<GetStateFn>(GetProcAddress(s_lib, "XInputGetState"));
                if (s_getState) return;
                FreeLibrary(s_lib); s_lib = nullptr;
            }
        }
    }

    static DWORD GetState(DWORD idx, XINPUT_STATE* st) {
        if (!s_getState) return ERROR_DEVICE_NOT_CONNECTED;
        return s_getState(idx, st);
    }
}

// ─── Key-state helpers (replace raylib IsKeyPressed / IsKeyDown) ─────────────
// We track "last frame" state ourselves so we can report pressed-this-frame.

static bool s_keyPrev[256] = {};
static bool s_keyCur [256] = {};

static void InputAdapter_PollKeys() {
    for (int i = 0; i < 256; i++) {
        s_keyPrev[i] = s_keyCur[i];
        s_keyCur [i] = (GetAsyncKeyState(i) & 0x8000) != 0;
    }
}

static bool KeyDown   (int vk) { return s_keyCur[vk & 0xFF]; }
static bool KeyPressed(int vk) { return  s_keyCur[vk & 0xFF] && !s_keyPrev[vk & 0xFF]; }

// ─── Gamepad button helpers ──────────────────────────────────────────────────
static XINPUT_STATE s_padPrev = {};
static XINPUT_STATE s_padCur  = {};
static int          s_padId   = 0;

static void InputAdapter_PollPad() {
    s_padPrev = s_padCur;
    // Auto-detect first connected pad
    for (int i = 0; i < 4; i++) {
        XINPUT_STATE st = {};
        if (XInputLocal::GetState(i, &st) == ERROR_SUCCESS) {
            s_padId  = i;
            s_padCur = st;
            return;
        }
    }
    // No pad connected — zero it out
    s_padCur = {};
}

static bool BtnDown   (WORD btn) { return (s_padCur.Gamepad.wButtons  & btn) != 0; }
static bool BtnPressed(WORD btn) {
    return (s_padCur.Gamepad.wButtons & btn) != 0 &&
           (s_padPrev.Gamepad.wButtons & btn) == 0;
}

static float AxisNorm(SHORT raw) { return raw / 32767.0f; }

// ─── InputAdapter class ──────────────────────────────────────────────────────

class InputAdapter {
public:
    float stickTimer      = 0.0f;
    float lastFrameTime   = 0.0f;   // seconds, updated by Update()
    const float STICK_DELAY = 0.22f;
    const float DEADZONE    = 0.50f;

    void Init() {
        XInputLocal::Load();
        // Initialise QPC timer
        QueryPerformanceFrequency(&m_freq);
        QueryPerformanceCounter(&m_last);
    }

    // Call at the very start of your main loop
    void Update() {
        // Compute delta time via QPC
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        lastFrameTime = static_cast<float>(
            (now.QuadPart - m_last.QuadPart) / (double)m_freq.QuadPart);
        m_last = now;

        InputAdapter_PollKeys();
        InputAdapter_PollPad();

        if (stickTimer > 0.f) stickTimer -= lastFrameTime;
    }

    // ── Navigation ───────────────────────────────────────────────────────────

    bool IsMoveDown() {
        if (KeyPressed(VK_DOWN) || KeyPressed('S')) return true;
        if (BtnPressed(XINPUT_GAMEPAD_DPAD_DOWN))   return true;
        float ay = AxisNorm(s_padCur.Gamepad.sThumbLY);
        if (ay < -DEADZONE && stickTimer <= 0.f) { stickTimer = STICK_DELAY; return true; }
        return false;
    }

    bool IsMoveUp() {
        if (KeyPressed(VK_UP) || KeyPressed('W'))  return true;
        if (BtnPressed(XINPUT_GAMEPAD_DPAD_UP))    return true;
        float ay = AxisNorm(s_padCur.Gamepad.sThumbLY);
        if (ay > DEADZONE && stickTimer <= 0.f) { stickTimer = STICK_DELAY; return true; }
        return false;
    }

    bool IsMoveLeft() {
        if (KeyPressed(VK_LEFT) || KeyPressed('A')) return true;
        if (BtnPressed(XINPUT_GAMEPAD_DPAD_LEFT))   return true;
        float ax = AxisNorm(s_padCur.Gamepad.sThumbLX);
        if (ax < -DEADZONE && stickTimer <= 0.f) { stickTimer = STICK_DELAY; return true; }
        return false;
    }

    bool IsMoveRight() {
        if (KeyPressed(VK_RIGHT) || KeyPressed('D')) return true;
        if (BtnPressed(XINPUT_GAMEPAD_DPAD_RIGHT))   return true;
        float ax = AxisNorm(s_padCur.Gamepad.sThumbLX);
        if (ax > DEADZONE && stickTimer <= 0.f) { stickTimer = STICK_DELAY; return true; }
        return false;
    }

    // ── Actions ───────────────────────────────────────────────────────────────

    // Confirm: Enter / Space / Xbox A (bottom face)
    bool IsConfirm() {
        return KeyPressed(VK_RETURN) || KeyPressed(VK_SPACE) ||
               BtnPressed(XINPUT_GAMEPAD_A);
    }

    // Back: Backspace / Escape / Xbox B (right face)
    bool IsBack() {
        return KeyPressed(VK_BACK) || KeyPressed(VK_ESCAPE) ||
               BtnPressed(XINPUT_GAMEPAD_B);
    }

    // Change art: Y key / Xbox Y (top face)
    bool IsChangeArt() {
        return KeyPressed('Y') || BtnPressed(XINPUT_GAMEPAD_Y);
    }

    // Delete hold: H key / Xbox X (left face) — held down
    bool IsDeleteDown() {
        return KeyDown('H') || BtnDown(XINPUT_GAMEPAD_X);
    }

    // Delete released
    bool IsDeleteReleased() {
        return (!KeyDown('H')             && s_keyPrev['H'])  ||
               (!BtnDown(XINPUT_GAMEPAD_X) && (s_padPrev.Gamepad.wButtons & XINPUT_GAMEPAD_X));
    }

    // ── Visual ────────────────────────────────────────────────────────────────

    // Hide focus ring if mouse moved significantly
    bool ShouldShowFocus() {
        static POINT lastPos = {};
        POINT cur;  GetCursorPos(&cur);
        int dx = cur.x - lastPos.x, dy = cur.y - lastPos.y;
        lastPos = cur;
        return (std::abs(dx) <= 1 && std::abs(dy) <= 1);
    }

    // ── Adaptive FPS helper ───────────────────────────────────────────────────

    bool HasAnyInput() {
        // Keyboard common keys
        static const int checkKeys[] = {
            VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
            'W', 'A', 'S', 'D',
            VK_RETURN, VK_SPACE, VK_ESCAPE, VK_BACK,
            VK_TAB, VK_F1, 'B', 'P', 'Y', 'H', 'X', 'O'
        };
        for (int k : checkKeys)
            if (KeyDown(k)) return true;

        // Gamepad buttons
        WORD allBtns =
            s_padCur.Gamepad.wButtons;
        if (allBtns) return true;

        // Sticks / triggers
        if (std::abs(AxisNorm(s_padCur.Gamepad.sThumbLX)) > DEADZONE) return true;
        if (std::abs(AxisNorm(s_padCur.Gamepad.sThumbLY)) > DEADZONE) return true;
        if (std::abs(AxisNorm(s_padCur.Gamepad.sThumbRX)) > DEADZONE) return true;
        if (std::abs(AxisNorm(s_padCur.Gamepad.sThumbRY)) > DEADZONE) return true;
        if (s_padCur.Gamepad.bLeftTrigger  > 26) return true;
        if (s_padCur.Gamepad.bRightTrigger > 26) return true;

        // Mouse movement
        POINT cur; GetCursorPos(&cur);
        static POINT prev = {};
        bool moved = (std::abs(cur.x - prev.x) > 2 || std::abs(cur.y - prev.y) > 2);
        prev = cur;
        if (moved) return true;

        // Mouse buttons
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return true;
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) return true;
        if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) return true;

        return false;
    }

private:
    LARGE_INTEGER m_freq = {};
    LARGE_INTEGER m_last = {};
};
