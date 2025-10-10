/***************************************************************************
    Force Feedback (aka Haptic) Support — patched
    
    Linux: uses evdev (/dev/input/event*) with capability checks, stable
           device open/close, and a small bank of pre-uploaded effects.
    Windows: uses DirectInput 8; picks device via env var FF_TARGET_VIDPID
             (e.g., "0x046d:0xc24f") or first FF-capable device; non-exclusive
             cooperative level to avoid SDL conflicts; reacquires on loss.

    Copyright (c) 2025
***************************************************************************/

#include "ffeedback.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifdef __linux__
// --------------------------- Linux (evdev) ---------------------------
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <vector>
#include <string>


namespace forcefeedback {

static int fd = -1;
static bool g_supported = false;
static struct ff_effect effects[6]; // 0 unused, 1..5 = soft..strong (or vice versa)

static bool has_bit(const unsigned long* bits, int bit)
{
    return (bits[bit / (sizeof(unsigned long)*8)] >> (bit % (sizeof(unsigned long)*8))) & 1UL;
}

bool init(int max_force, int min_force, int duration_ms)
{
    if (fd >= 0) { g_supported = true; return true; }

    const char* devpath = "/dev/input/event";
    char device_file_name[64] = {0};

    for (int idx = 0; idx < 100; ++idx)
    {
        std::snprintf(device_file_name, sizeof(device_file_name), "%s%d", devpath, idx);
        int tmp = ::open(device_file_name, O_RDWR | O_CLOEXEC);
        if (tmp < 0) continue;

        // check this is a FF-capable device
        const size_t __BPL = sizeof(unsigned long) * 8;
        unsigned long ev_bits[(EV_MAX + __BPL - 1) / __BPL] = {};
        if (ioctl(tmp, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) == -1) { ::close(tmp); continue; }
        if (!has_bit(ev_bits, EV_FF)) { ::close(tmp); continue; }

        unsigned long ff_bits[(FF_MAX + __BPL - 1) / __BPL] = {};
        if (ioctl(tmp, EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits) == -1) { ::close(tmp); continue; }
        bool have_rumble  = has_bit(ff_bits, FF_RUMBLE);
        bool have_period  = has_bit(ff_bits, FF_PERIODIC);
        if (!have_rumble && !have_period) { ::close(tmp); continue; }

        fd = tmp;
        break;
    }

    if (fd < 0) { g_supported = false; return false; }

    // Attempt to set overall gain (ignore failures)
    struct input_event ie{};
    ie.type = EV_FF;
    ie.code = FF_GAIN;
    ie.value = 0x7fff;
    (void)write(fd, &ie, sizeof(ie));

    // Upload a small bank of effects: 1..5
    for (int j = 1; j <= 5; ++j)
    {
        struct ff_effect e{};
        e.type = FF_RUMBLE;
        e.id = -1;
        e.u.rumble.strong_magnitude = (unsigned short)std::max(0, std::min(0x7fff, max_force - (j-1) * (max_force - min_force) / 4));
        e.u.rumble.weak_magnitude   = e.u.rumble.strong_magnitude / 2;
        e.replay.length = std::max(10, duration_ms);
        e.replay.delay  = 0;

        if (ioctl(fd, EVIOCSFF, &e) == -1)
        {
            // try periodic sine as fallback
            std::memset(&e, 0, sizeof(e));
            e.type = FF_PERIODIC;
            e.id = -1;
            e.u.periodic.waveform = FF_SINE;
            e.u.periodic.magnitude = (unsigned short)std::max(0, std::min(0x7fff, max_force - (j-1) * (max_force - min_force) / 4));
            e.u.periodic.period = 50;
            e.u.periodic.offset = 0;
            e.u.periodic.phase = 0;
            e.replay.length = std::max(10, duration_ms);
            e.replay.delay  = 0;

            if (ioctl(fd, EVIOCSFF, &e) == -1)
            {
                // give up on this device
                ::close(fd); fd = -1; g_supported = false;
                return false;
            }
        }
        effects[j] = e;
    }

    g_supported = true;
    return true;
}

int set(int command, int force) // command is unused; keep for ABI compatibility
{
    if (!g_supported || fd < 0) return -1;
    int idx = std::max(1, std::min(5, force));

    struct input_event play{};
    play.type = EV_FF;
    play.code = effects[idx].id;
    play.value = 1;
    if (write(fd, &play, sizeof(play)) == -1) return -1;

    return 0;
}

void close()
{
    if (fd >= 0)
    {
        // Try to stop all uploaded effects
        for (int j = 1; j <= 5; ++j)
        {
            if (effects[j].id >= 0)
            {
                struct input_event stop{};
                stop.type = EV_FF;
                stop.code = effects[j].id;
                stop.value = 0;
                (void)write(fd, &stop, sizeof(stop));
            }
        }
        ::close(fd);
        fd = -1;
    }
    g_supported = false;
}

bool is_supported()
{
    return g_supported;
}

} // namespace forcefeedback

#elif defined(_WIN32)
// --------------------------- Windows (DirectInput 8) ---------------------------
#define DIRECTINPUT_VERSION 0x0800
#define NOMINMAX
#include <windows.h>
#include <dinput.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace forcefeedback {

static LPDIRECTINPUT8       g_pDI        = nullptr;
static LPDIRECTINPUTDEVICE8 g_pDevice    = nullptr;
static LPDIRECTINPUTEFFECT  g_pEffect    = nullptr;
static bool                 g_supported  = false;

static unsigned short parse_hex4(const char* s) {
    unsigned v=0; if (!s) return 0;
    std::sscanf(s, "%x", &v); return (unsigned short)v;
}

static bool match_target_vidpid(LPDIRECTINPUTDEVICE8 dev)
{
    // Use DIPROP_VIDPID to match against env var FF_TARGET_VIDPID="0xAAAA:0xBBBB"
    char* env = std::getenv("FF_TARGET_VIDPID");
    if (!env) return true; // no filter set

    DIPROPDWORD dp; std::memset(&dp, 0, sizeof(dp));
    dp.diph.dwSize = sizeof(DIPROPDWORD);
    dp.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dp.diph.dwHow = DIPH_DEVICE;

    if (FAILED(dev->GetProperty(DIPROP_VIDPID, &dp.diph))) return true; // can't read, allow

    unsigned short vid = LOWORD(dp.dwData);
    unsigned short pid = HIWORD(dp.dwData);

    unsigned short tvid = 0, tpid = 0;
    const char* colon = std::strchr(env, ':');
    if (colon) {
        tvid = parse_hex4(env);
        tpid = parse_hex4(colon + 1);
    }
    return (!tvid || tvid == vid) && (!tpid || tpid == pid);
}

static BOOL CALLBACK EnumFFDevicesCallback(const DIDEVICEINSTANCE* pdidInstance, VOID*)
{
    if (!g_pDI || g_pDevice) return DIENUM_CONTINUE;

    LPDIRECTINPUTDEVICE8 dev = nullptr;
    if (FAILED(g_pDI->CreateDevice(pdidInstance->guidInstance, &dev, nullptr)))
        return DIENUM_CONTINUE;

    if (!match_target_vidpid(dev)) { dev->Release(); return DIENUM_CONTINUE; }

    if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2))) { dev->Release(); return DIENUM_CONTINUE; }

    // Non-exclusive, foreground to avoid conflicts with SDL/XInput
    HWND hwnd = GetForegroundWindow();
    if (FAILED(dev->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND))) {
        dev->Release(); return DIENUM_CONTINUE;
    }

    // Query FFB support
    DIDEVCAPS caps; std::memset(&caps, 0, sizeof(caps)); caps.dwSize = sizeof(caps);
    if (FAILED(dev->GetCapabilities(&caps)) || !(caps.dwFlags & DIDC_FORCEFEEDBACK)) {
        dev->Release(); return DIENUM_CONTINUE;
    }

    // Try to create a constant force effect; fall back to sine if needed
    DIEFFECT eff; std::memset(&eff, 0, sizeof(eff));
    LONG lDirection[2] = { 0, 0 };
    DICONSTANTFORCE cf; std::memset(&cf, 0, sizeof(cf));
    cf.lMagnitude = DI_FFNOMINALMAX;

    eff.dwSize = sizeof(DIEFFECT);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.cAxes = 1;

    // JJP - this line doesn't compile on Windows
    // eff.rgdwAxes = (DWORD*)(& (DWORD){ DIJOFS_X });
    static const DWORD axisX = DIJOFS_X;          // storage for the value
    eff.rgdwAxes = const_cast<DWORD*>(&axisX);

    eff.rglDirection = lDirection;
    eff.lpEnvelope = 0;
    eff.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
    eff.lpvTypeSpecificParams = &cf;
    eff.dwDuration = INFINITE;
    eff.dwGain = DI_FFNOMINALMAX;

    LPDIRECTINPUTEFFECT effect = nullptr;
    HRESULT hr = dev->CreateEffect(GUID_ConstantForce, &eff, &effect, nullptr);
    if (FAILED(hr)) {
        // Try sine
        DIPERIODIC per; std::memset(&per, 0, sizeof(per));
        per.dwMagnitude = DI_FFNOMINALMAX;
        per.dwPeriod = (DWORD)(0.05f * DI_SECONDS);
        per.lOffset = 0;
        per.dwPhase = 0;

        eff.cbTypeSpecificParams = sizeof(DIPERIODIC);
        eff.lpvTypeSpecificParams = &per;
        hr = dev->CreateEffect(GUID_Sine, &eff, &effect, nullptr);
    }

    if (FAILED(hr)) {
        dev->Release(); return DIENUM_CONTINUE;
    }

    g_pDevice = dev;
    g_pEffect = effect;
    return DIENUM_STOP;
}

bool init(int, int, int)
{
    if (g_supported) return true;

    if (FAILED(DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&g_pDI, nullptr)))
        return false;

    if (FAILED(g_pDI->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumFFDevicesCallback, nullptr, DIEDFL_ATTACHEDONLY)))
        return false;

    if (!g_pDevice || !g_pEffect) return false;

    if (FAILED(g_pDevice->Acquire())) {
        // try later in set()
    }

    g_supported = true;
    return true;
}

static HRESULT start_or_reacquire(DIEFFECT* peff, DWORD flags)
{
    if (!g_pEffect || !g_pDevice) return E_FAIL;
    HRESULT r = g_pEffect->SetParameters(peff, flags);
    if (r == DIERR_INPUTLOST || r == DIERR_NOTACQUIRED) {
        if (SUCCEEDED(g_pDevice->Acquire())) {
            r = g_pEffect->SetParameters(peff, flags);
        }
    }
    return r;
}

int set(int, int force)
{
    if (!g_supported || !g_pEffect) return -1;

    // map 1..5 → 100%..20%
    force = (std::max)(1, (std::min)(5, force));
    //force = std::clamp(force, 1, 5);
    LONG mag = (LONG)(DI_FFNOMINALMAX * (1.0f - (force - 1) * 0.2f));

    DICONSTANTFORCE cf; std::memset(&cf, 0, sizeof(cf));
    cf.lMagnitude = mag;

    DIEFFECT eff; std::memset(&eff, 0, sizeof(eff));
    LONG lDirection[2] = { 0, 0 };

    eff.dwSize = sizeof(DIEFFECT);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.cAxes = 1;
    DWORD axis = DIJOFS_X;
    eff.rgdwAxes = &axis;
    eff.rglDirection = lDirection;
    eff.lpEnvelope = 0;
    eff.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
    eff.lpvTypeSpecificParams = &cf;
    eff.dwDuration = INFINITE;
    eff.dwGain = DI_FFNOMINALMAX;

    HRESULT hr = start_or_reacquire(&eff, DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS | DIEP_START);
    return FAILED(hr) ? -1 : 0;
}

void close()
{
    if (g_pEffect) { g_pEffect->Release(); g_pEffect = nullptr; }
    if (g_pDevice) { g_pDevice->Unacquire(); g_pDevice->Release(); g_pDevice = nullptr; }
    if (g_pDI)     { g_pDI->Release(); g_pDI = nullptr; }
    g_supported = false;
}

bool is_supported()
{
    return g_supported;
}

} // namespace forcefeedback

#else

// Stubs for other platforms
namespace forcefeedback {
bool init(int, int, int) { return false; }
int  set(int, int) { return -1; }
void close() {}
bool is_supported() { return false; }
}

#endif
