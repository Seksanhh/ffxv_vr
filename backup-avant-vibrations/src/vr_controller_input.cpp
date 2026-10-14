#include "vr_controller_input.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace VrInput
{
namespace
{
// Right stick deflection (0..1) that selects a D-pad direction while the
// weapon button is held, and the one below which it is let go: the gap keeps
// a slot from chattering at the edge.
constexpr float kDpadPress = 0.60f;
constexpr float kDpadRelease = 0.35f;
// After a chord the camera takes the right stick back only once the stick has
// returned to the centre, so letting go of the button never swings the view.
constexpr float kStickRecentred = 0.25f;
// Idle Touch sticks and triggers are not exactly zero.  Below these they read
// as untouched, so a controller lying on the desk sends nothing at all.
constexpr float kStickDeadzone = 0.08f;
constexpr float kTriggerDeadzone = 0.05f;
// Touch grips are analog: pressed above 0.55, released below 0.35.
constexpr float kGripPress = 0.55f;
constexpr float kGripRelease = 0.35f;
// A tap of the weapon button (released within kTapMs, no chord) still sends
// the button's own press, held kTapPulseMs so the game sees it.
constexpr std::uint64_t kTapMs = 350;
constexpr std::uint64_t kTapPulseMs = 120;
// The presenter publishes once per present.  A sample older than kStaleMs is
// never used (nothing stays pressed); the pad itself stays connected, idle,
// for kConnectedMs, so a loading-screen stall does not unplug it in the game.
constexpr std::uint64_t kStaleMs = 250;
constexpr std::uint64_t kConnectedMs = 5000;

constexpr WORD kDpadBits[4] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT};
constexpr const char* kWeaponButtonNames[kWeaponButtonCount] = {
    "left menu button", "Y", "X", "left grip"};

void Deadzone(float& x, float& y)
{
    if (!std::isfinite(x) || !std::isfinite(y) ||
        x * x + y * y < kStickDeadzone * kStickDeadzone)
        x = y = 0.0f;
}

SHORT ToStick(float value)
{
    const float v = std::clamp(value, -1.0f, 1.0f);
    return static_cast<SHORT>(v >= 0.0f ? std::lround(v * 32767.0f) : std::lround(v * 32768.0f));
}

BYTE ToTrigger(float value)
{
    if (!std::isfinite(value) || value < kTriggerDeadzone)
        return 0;
    return static_cast<BYTE>(std::lround(std::min(value, 1.0f) * 255.0f));
}

WORD OwnButton(WeaponButton button)
{
    switch (button)
    {
    case WeaponButton::Y: return XINPUT_GAMEPAD_Y;
    case WeaponButton::X: return XINPUT_GAMEPAD_X;
    case WeaponButton::LeftGrip: return XINPUT_GAMEPAD_LEFT_SHOULDER;
    default: return XINPUT_GAMEPAD_START;
    }
}

int Dominant(float x, float y)
{
    if (std::fabs(x) > std::fabs(y))
        return x > 0.0f ? 1 : 3;
    return y > 0.0f ? 0 : 2;
}

bool SamePad(const XINPUT_GAMEPAD& a, const XINPUT_GAMEPAD& b)
{
    return a.wButtons == b.wButtons && a.bLeftTrigger == b.bLeftTrigger &&
        a.bRightTrigger == b.bRightTrigger && a.sThumbLX == b.sThumbLX &&
        a.sThumbLY == b.sThumbLY && a.sThumbRX == b.sThumbRX && a.sThumbRY == b.sThumbRY;
}

// Real pad and controllers together: buttons add up, the deeper trigger and
// the further deflected stick win.
XINPUT_GAMEPAD Merge(const XINPUT_GAMEPAD& real, const XINPUT_GAMEPAD& mapped)
{
    const auto magnitude = [](SHORT x, SHORT y)
    {
        return static_cast<long long>(x) * x + static_cast<long long>(y) * y;
    };
    XINPUT_GAMEPAD out = real;
    out.wButtons = static_cast<WORD>(real.wButtons | mapped.wButtons);
    out.bLeftTrigger = std::max(real.bLeftTrigger, mapped.bLeftTrigger);
    out.bRightTrigger = std::max(real.bRightTrigger, mapped.bRightTrigger);
    if (magnitude(mapped.sThumbLX, mapped.sThumbLY) > magnitude(real.sThumbLX, real.sThumbLY))
    {
        out.sThumbLX = mapped.sThumbLX;
        out.sThumbLY = mapped.sThumbLY;
    }
    if (magnitude(mapped.sThumbRX, mapped.sThumbRY) > magnitude(real.sThumbRX, real.sThumbRY))
    {
        out.sThumbRX = mapped.sThumbRX;
        out.sThumbRY = mapped.sThumbRY;
    }
    return out;
}

// Presenter side.
Mapper g_mapper;                                // presenter thread only
WeaponButton g_mappedButton{WeaponButton::Menu}; // presenter thread only
bool g_loggedActive{};                          // presenter thread only
SRWLOCK g_padLock = SRWLOCK_INIT;
XINPUT_GAMEPAD g_pad{};                         // under g_padLock
bool g_padActive{};                             // under g_padLock
bool g_controllersSeen{};                       // under g_padLock
std::uint64_t g_padMs{};                        // under g_padLock

// XInput side.  The packet number changes exactly when the returned state
// does, as XInput's does; games skip a packet number they have already seen.
SRWLOCK g_mergeLock = SRWLOCK_INIT;
XINPUT_GAMEPAD g_lastOut{};                     // under g_mergeLock
bool g_lastOutValid{};                          // under g_mergeLock
DWORD g_packet = 0x00010000;                    // under g_mergeLock

std::atomic_bool g_enabled{true};
std::atomic_int g_weaponButton{0};
std::atomic_bool g_vrKey{};
std::atomic_uint64_t g_lastGameReadMs{};
} // namespace

MapResult Mapper::Update(const Controllers& in, std::uint64_t nowMs, WeaponButton weaponButton)
{
    MapResult result{};
    if (!in.active)
    {
        Reset();
        return result;
    }
    leftGrip_ = in.leftGrip > (leftGrip_ ? kGripRelease : kGripPress);
    rightGrip_ = in.rightGrip > (rightGrip_ ? kGripRelease : kGripPress);
    const bool weaponDown = weaponButton == WeaponButton::Y ? in.y
        : weaponButton == WeaponButton::X ? in.x
        : weaponButton == WeaponButton::LeftGrip ? leftGrip_
        : in.menu;
    if (weaponDown && !weaponHeld_)
    {
        weaponHeld_ = true;
        chordUsed_ = false;
        weaponSinceMs_ = nowMs;
    }
    else if (!weaponDown && weaponHeld_)
    {
        weaponHeld_ = false;
        if (!chordUsed_ && nowMs - weaponSinceMs_ <= kTapMs)
        {
            pulseButton_ = OwnButton(weaponButton);
            pulseUntilMs_ = nowMs + kTapPulseMs;
        }
    }

    float lx = in.leftX, ly = in.leftY, rx = in.rightX, ry = in.rightY;
    Deadzone(lx, ly);
    Deadzone(rx, ry);
    const float rightDeflection = std::sqrt(rx * rx + ry * ry);
    const bool leftClickPressed = in.leftStickClick && !leftClickWas_;
    const bool rightClickPressed = in.rightStickClick && !rightClickWas_;
    leftClickWas_ = in.leftStickClick;
    rightClickWas_ = in.rightStickClick;

    WORD buttons = 0;
    if (weaponHeld_)
    {
        // A slot once the stick is pushed past kDpadPress, kept until it
        // falls back below kDpadRelease; a full push another way moves
        // straight to that slot.
        if (rightDeflection < kDpadRelease)
            dpad_ = -1;
        else if (rightDeflection >= kDpadPress)
            dpad_ = Dominant(rx, ry);
        if (dpad_ >= 0)
            chordUsed_ = true;
        rightStickLocked_ = true;
        if (rightClickPressed)
            rightClickLocked_ = true;
        if (rightClickLocked_ && in.rightStickClick)
        {
            buttons |= XINPUT_GAMEPAD_BACK;
            chordUsed_ = true;
        }
        if (leftClickPressed)
        {
            leftClickLocked_ = true;
            result.vrKey = true;
            chordUsed_ = true;
        }
    }
    else
    {
        dpad_ = -1;
        if (rightDeflection < kStickRecentred)
            rightStickLocked_ = false;
    }
    if (!in.rightStickClick)
        rightClickLocked_ = false;
    if (!in.leftStickClick)
        leftClickLocked_ = false;

    XINPUT_GAMEPAD& pad = result.pad;
    pad.sThumbLX = ToStick(lx);
    pad.sThumbLY = ToStick(ly);
    if (!rightStickLocked_)
    {
        pad.sThumbRX = ToStick(rx);
        pad.sThumbRY = ToStick(ry);
    }
    pad.bLeftTrigger = ToTrigger(in.leftTrigger);
    pad.bRightTrigger = ToTrigger(in.rightTrigger);
    if (in.a)
        buttons |= XINPUT_GAMEPAD_A;
    if (in.b)
        buttons |= XINPUT_GAMEPAD_B;
    if (in.x && weaponButton != WeaponButton::X)
        buttons |= XINPUT_GAMEPAD_X;
    if (in.y && weaponButton != WeaponButton::Y)
        buttons |= XINPUT_GAMEPAD_Y;
    if (leftGrip_ && weaponButton != WeaponButton::LeftGrip)
        buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (rightGrip_)
        buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (in.menu && weaponButton != WeaponButton::Menu)
        buttons |= XINPUT_GAMEPAD_START;
    if (in.leftStickClick && !leftClickLocked_)
        buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (in.rightStickClick && !rightClickLocked_)
        buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (dpad_ >= 0)
        buttons |= kDpadBits[dpad_];
    if (nowMs < pulseUntilMs_)
        buttons |= pulseButton_;
    pad.wButtons = buttons;
    return result;
}

void Mapper::Reset()
{
    *this = Mapper{};
}

void Publish(const Controllers& in)
{
    const std::uint64_t nowMs = GetTickCount64();
    const bool use = in.active && g_enabled.load(std::memory_order_relaxed);
    const auto button = static_cast<WeaponButton>(g_weaponButton.load(std::memory_order_relaxed));
    if (button != g_mappedButton)
    {
        g_mapper.Reset();
        g_mappedButton = button;
    }
    MapResult mapped{};
    if (use)
        mapped = g_mapper.Update(in, nowMs, button);
    else
        g_mapper.Reset();
    if (mapped.vrKey)
        g_vrKey.store(true, std::memory_order_release);
    AcquireSRWLockExclusive(&g_padLock);
    g_pad = mapped.pad;
    g_padActive = use;
    g_controllersSeen = in.active;
    g_padMs = nowMs;
    ReleaseSRWLockExclusive(&g_padLock);
    if (use != g_loggedActive)
    {
        g_loggedActive = use;
        char line[192];
        std::snprintf(line, sizeof(line), use
            ? "[VRPAD] controllers drive the gamepad; weapons: hold the %s and push the right stick"
            : "[VRPAD] controllers no longer drive the gamepad (%s)",
            use ? kWeaponButtonNames[static_cast<int>(button)]
                : (in.active ? "turned off in the menu" : "not focused or no controller bound"));
        RetailLogLine(line);
    }
}

bool ApplyToState(DWORD user, DWORD realResult, XINPUT_STATE* state, bool blocked)
{
    if (user != 0 || !state)
        return false;
    const std::uint64_t nowMs = GetTickCount64();
    AcquireSRWLockShared(&g_padLock);
    const bool active = g_padActive && g_padMs + kConnectedMs >= nowMs;
    const XINPUT_GAMEPAD mapped = g_padMs + kStaleMs >= nowMs ? g_pad : XINPUT_GAMEPAD{};
    ReleaseSRWLockShared(&g_padLock);
    if (!active)
        return false;
    const XINPUT_GAMEPAD out = blocked ? XINPUT_GAMEPAD{}
        : realResult == ERROR_SUCCESS ? Merge(state->Gamepad, mapped) : mapped;
    AcquireSRWLockExclusive(&g_mergeLock);
    if (!g_lastOutValid || !SamePad(out, g_lastOut))
    {
        ++g_packet;
        g_lastOut = out;
        g_lastOutValid = true;
    }
    state->dwPacketNumber = g_packet;
    ReleaseSRWLockExclusive(&g_mergeLock);
    state->Gamepad = out;
    g_lastGameReadMs.store(nowMs, std::memory_order_relaxed);
    return true;
}

bool ProvidesPad(DWORD user)
{
    if (user != 0)
        return false;
    const std::uint64_t nowMs = GetTickCount64();
    AcquireSRWLockShared(&g_padLock);
    const bool active = g_padActive && g_padMs + kConnectedMs >= nowMs;
    ReleaseSRWLockShared(&g_padLock);
    return active;
}

bool ConsumeVrKey()
{
    return g_vrKey.exchange(false, std::memory_order_acq_rel);
}

void SetEnabled(bool enabled)
{
    if (g_enabled.exchange(enabled, std::memory_order_relaxed) != enabled)
        RetailLogLine(enabled ? "[VRPAD] VR controllers as gamepad ON"
                              : "[VRPAD] VR controllers as gamepad OFF");
}

bool GetEnabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void SetWeaponButton(int button)
{
    const int value = std::clamp(button, 0, kWeaponButtonCount - 1);
    if (g_weaponButton.exchange(value, std::memory_order_relaxed) != value)
    {
        char line[96];
        std::snprintf(line, sizeof(line), "[VRPAD] weapon button = %s", kWeaponButtonNames[value]);
        RetailLogLine(line);
    }
}

int GetWeaponButton()
{
    return g_weaponButton.load(std::memory_order_relaxed);
}

bool ControllersActive()
{
    const std::uint64_t nowMs = GetTickCount64();
    AcquireSRWLockShared(&g_padLock);
    const bool seen = g_controllersSeen && g_padMs + kStaleMs >= nowMs;
    ReleaseSRWLockShared(&g_padLock);
    return seen;
}

bool GameReading()
{
    const std::uint64_t last = g_lastGameReadMs.load(std::memory_order_relaxed);
    return last != 0 && last + 1000 >= GetTickCount64();
}
} // namespace VrInput
