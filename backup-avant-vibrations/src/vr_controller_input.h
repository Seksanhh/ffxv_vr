#pragma once
// [VRPAD] VR controllers (Meta Quest Touch) as the game's gamepad.
//
// The game reads one Xbox pad through XInputGetState, which aer_menu.cpp
// already hooks.  The presenter reads the controllers through OpenXR actions
// once per present and publishes a sample here; the XInput hook merges the
// mapped pad into whatever a real pad reports, or stands in for pad 0 when no
// real pad is connected.
//
// Touch maps one to one onto the pad except for two things the game needs:
// the D-pad (the four weapon slots) and Back.  Both come from a chord: hold
// the weapon button (the left menu button by default) and
//   push the right stick up/right/down/left -> D-pad up/right/down/left;
//   click the right stick                   -> Back;
//   click the left stick                    -> the VR key (start VR / recenter).
// The camera ignores the right stick while the weapon button is held, and a
// short tap of the weapon button still does that button's own job.
#include <windows.h>
#include <xinput.h>

#include <cstdint>

namespace VrInput
{
// One OpenXR sample.  Sticks are -1..1 with +y up; triggers and grips 0..1.
struct Controllers
{
    bool active{};              // session focused and a controller bound
    float leftX{}, leftY{}, rightX{}, rightY{};
    float leftTrigger{}, rightTrigger{};
    float leftGrip{}, rightGrip{};
    bool a{}, b{}, x{}, y{};
    bool leftStickClick{}, rightStickClick{};
    bool menu{};                // left controller menu button
};

// The button held to turn the right stick into the D-pad.
enum class WeaponButton : int
{
    Menu = 0,
    Y = 1,
    X = 2,
    LeftGrip = 3,
};
constexpr int kWeaponButtonCount = 4;

struct MapResult
{
    XINPUT_GAMEPAD pad{};
    bool vrKey{};               // weapon button + left stick click, rising edge
};

// The mapping state machine.  Publish() drives the one live instance; it is a
// class so the timing rules can be exercised without a headset.
class Mapper
{
public:
    MapResult Update(const Controllers& in, std::uint64_t nowMs, WeaponButton weaponButton);
    void Reset();

private:
    bool weaponHeld_{};
    bool chordUsed_{};
    std::uint64_t weaponSinceMs_{};
    WORD pulseButton_{};
    std::uint64_t pulseUntilMs_{};
    int dpad_{-1};              // 0 up, 1 right, 2 down, 3 left
    bool rightStickLocked_{};   // camera held until the right stick re-centres
    bool rightClickLocked_{};   // right stick click consumed by the chord
    bool leftClickLocked_{};    // left stick click consumed by the chord
    bool leftClickWas_{}, rightClickWas_{};
    bool leftGrip_{}, rightGrip_{};
};

// Presenter thread, once per present (an inactive sample when the session is
// not focused or no controller is bound).
void Publish(const Controllers& in);

// XInput hook, any thread.  Merges the mapped pad into pad 0, or stands in
// for it; true when the call must then report ERROR_SUCCESS.  blocked (the
// mod's menu is open) returns an idle pad, counted in the packet number.
bool ApplyToState(DWORD user, DWORD realResult, XINPUT_STATE* state, bool blocked = false);
// True while the mapped pad can stand in for this pad index.
bool ProvidesPad(DWORD user);

// Set by the chord, consumed once by the present thread (main.cpp Tick).
bool ConsumeVrKey();

void SetEnabled(bool enabled);
bool GetEnabled();
void SetWeaponButton(int button);
int GetWeaponButton();
// Controllers published as active within the last quarter second.
bool ControllersActive();
// The game read the mapped pad within the last second.
bool GameReading();
}
