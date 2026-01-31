// ============================================================================
// INPUT ADAPTER - MERGED VERSION (Your style + HasAnyInput for adaptive FPS)
// File: input.cpp
// ============================================================================

#pragma once
#include "raylib.h"
#include <cmath>

class InputAdapter {
public:
    int gamepadID = 0;               // Standard ID for the first controller
    float stickTimer = 0.0f;         // Prevents menu-scrolling too fast
    const float STICK_DELAY = 0.22f; // Time in seconds between scroll steps
    const float DEADZONE = 0.5f;     // High deadzone to ignore stick drift

    // Call this at the very beginning of your main loop
    void Update() {
        if (stickTimer > 0) {
            stickTimer -= GetFrameTime();
        }
        
        // Auto-detect first available gamepad
        for (int i = 0; i < 4; i++) {
            if (IsGamepadAvailable(i)) {
                gamepadID = i;
                break;
            }
        }
    }

    // --- NAVIGATION LOGIC ---

    bool IsMoveDown() {
        // 1. Check Keyboard (Arrow + WASD)
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) return true;
        
        // 2. Check D-Pad (Left Face Down)
        if (IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) return true;
        
        // 3. Check Analog Stick (With timer to prevent infinite speed)
        float axisY = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_Y);
        if (axisY > DEADZONE && stickTimer <= 0) {
            stickTimer = STICK_DELAY;
            return true;
        }
        return false;
    }

    bool IsMoveUp() {
        // 1. Check Keyboard
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) return true;
        
        // 2. Check D-Pad (Left Face Up)
        if (IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_FACE_UP)) return true;
        
        // 3. Check Analog Stick
        float axisY = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_Y);
        if (axisY < -DEADZONE && stickTimer <= 0) {
            stickTimer = STICK_DELAY;
            return true;
        }
        return false;
    }

    bool IsMoveLeft() {
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) return true;
        if (IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) return true;
        
        float axisX = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_X);
        if (axisX < -DEADZONE && stickTimer <= 0) {
            stickTimer = STICK_DELAY;
            return true;
        }
        return false;
    }

    bool IsMoveRight() {
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) return true;
        if (IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) return true;
        
        float axisX = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_X);
        if (axisX > DEADZONE && stickTimer <= 0) {
            stickTimer = STICK_DELAY;
            return true;
        }
        return false;
    }

    // --- ACTION LOGIC ---

    // Standard Confirm: Enter/Space or Xbox (A) / PS (Cross)
    bool IsConfirm() {
        return (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
    }

    // Standard Back: Backspace/Escape or Xbox (B) / PS (Circle)
    bool IsBack() {
        return (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE) ||
                IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
    }

    // Custom Art: Y key or Xbox (Y) / PS (Triangle)
    bool IsChangeArt() {
        return (IsKeyPressed(KEY_Y) || 
                IsGamepadButtonPressed(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_UP));
    }

    // Delete/Hold Button: H key or Xbox (X) / PS (Square)
    bool IsDeleteDown() {
        return (IsKeyDown(KEY_H) || 
                IsGamepadButtonDown(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_LEFT));
    }

    bool IsDeleteReleased() {
        return (IsKeyReleased(KEY_H) || 
                IsGamepadButtonReleased(gamepadID, GAMEPAD_BUTTON_RIGHT_FACE_LEFT));
    }

    // --- VISUAL LOGIC ---
    
    // Hide focus borders if the user is moving the mouse instead
    bool ShouldShowFocus() {
        Vector2 mouseDelta = GetMouseDelta();
        if (fabs(mouseDelta.x) > 0.1f || fabs(mouseDelta.y) > 0.1f) return false;
        return true;
    }

    // =========================================================================
    // NEW: Required for Adaptive FPS system
    // =========================================================================
    
    // Check if ANY input is happening (for adaptive FPS)
    // Returns true if user is actively using the controller/keyboard
    bool HasAnyInput() {
        // Check common keyboard keys
        static const int checkKeys[] = {
            KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
            KEY_W, KEY_A, KEY_S, KEY_D,
            KEY_ENTER, KEY_SPACE, KEY_ESCAPE, KEY_BACKSPACE,
            KEY_TAB, KEY_F1, KEY_B, KEY_P, KEY_Y, KEY_H, KEY_X, KEY_O
        };
        
        for (int key : checkKeys) {
            if (IsKeyPressed(key) || IsKeyDown(key)) return true;
        }
        
        // Check gamepad if available
        if (IsGamepadAvailable(gamepadID)) {
            // Check all standard buttons
            for (int btn = 0; btn < 18; btn++) {
                if (IsGamepadButtonPressed(gamepadID, btn) || 
                    IsGamepadButtonDown(gamepadID, btn)) {
                    return true;
                }
            }
            
            // Check left analog stick
            float axisX = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_X);
            float axisY = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_Y);
            if (fabs(axisX) > DEADZONE || fabs(axisY) > DEADZONE) {
                return true;
            }
            
            // Check right analog stick
            float rx = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_RIGHT_X);
            float ry = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_RIGHT_Y);
            if (fabs(rx) > DEADZONE || fabs(ry) > DEADZONE) {
                return true;
            }
            
            // Check triggers
            float lt = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_LEFT_TRIGGER);
            float rt = GetGamepadAxisMovement(gamepadID, GAMEPAD_AXIS_RIGHT_TRIGGER);
            if (lt > 0.1f || rt > 0.1f) {
                return true;
            }
        }
        
        // Check mouse movement (with threshold to ignore tiny jitters)
        Vector2 mouseDelta = GetMouseDelta();
        if (fabs(mouseDelta.x) > 2.0f || fabs(mouseDelta.y) > 2.0f) {
            return true;
        }
        
        // Check mouse buttons
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || 
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
            IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
            return true;
        }
        
        return false;
    }
};