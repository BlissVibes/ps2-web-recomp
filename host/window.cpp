#include "window.hpp"

#include "ps2/system.hpp"

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

enum Button : std::uint16_t {
    kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3,
    kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
    kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
    kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15,
};

struct KeyBinding {
    SDL_Scancode key;
    std::uint16_t button;
};

constexpr KeyBinding kKeys[] = {
    {SDL_SCANCODE_UP, kUp},         {SDL_SCANCODE_DOWN, kDown},     {SDL_SCANCODE_LEFT, kLeft},
    {SDL_SCANCODE_RIGHT, kRight},   {SDL_SCANCODE_Z, kCross},       {SDL_SCANCODE_X, kCircle},
    {SDL_SCANCODE_A, kSquare},      {SDL_SCANCODE_S, kTriangle},    {SDL_SCANCODE_Q, kL1},
    {SDL_SCANCODE_W, kR1},          {SDL_SCANCODE_1, kL2},          {SDL_SCANCODE_2, kR2},
    {SDL_SCANCODE_RETURN, kStart},  {SDL_SCANCODE_BACKSPACE, kSelect},
};

struct PadBinding {
    SDL_GameControllerButton button;
    std::uint16_t mask;
};

constexpr PadBinding kPadButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, kCross},          {SDL_CONTROLLER_BUTTON_B, kCircle},
    {SDL_CONTROLLER_BUTTON_X, kSquare},         {SDL_CONTROLLER_BUTTON_Y, kTriangle},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL1},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR1},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, kL3},     {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kR3},
    {SDL_CONTROLLER_BUTTON_BACK, kSelect},      {SDL_CONTROLLER_BUTTON_START, kStart},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight},
};

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
SDL_AudioDeviceID g_audio = 0;
SDL_GameController *g_pad = nullptr;
std::uint32_t g_tex_w = 0, g_tex_h = 0;
std::uint64_t g_last = 0;
std::vector<std::uint32_t> g_pixels;
bool g_gpu_tried = false, g_gpu_present = false;

bool create_renderer() {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool attach_gpu() {
    if (std::getenv("PS2_SDL_RENDERER")) return false;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info)) return false;
    ps2::WindowTarget target;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    target.width = static_cast<std::uint32_t>(w);
    target.height = static_cast<std::uint32_t>(h);
    switch (info.subsystem) {
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        target.kind = ps2::WindowTarget::X11;
        target.display = info.info.x11.display;
        target.window = static_cast<std::uint64_t>(info.info.x11.window);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        target.kind = ps2::WindowTarget::Wayland;
        target.display = info.info.wl.display;
        target.surface = info.info.wl.surface;
        break;
#endif
    default: return false;
    }
    return ps2::attach_window(target);
}

std::uint8_t axis(SDL_GameControllerAxis a) {
    const int v = SDL_GameControllerGetAxis(g_pad, a);
    return static_cast<std::uint8_t>(std::clamp((v + 32768) >> 8, 0, 255));
}

void poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) ps2::halt("window closed");
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && g_gpu_present) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
            ps2::resize_window(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
            SDL_GameControllerClose(g_pad);
            g_pad = nullptr;
        }
    }
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    std::uint16_t pressed = 0;
    for (const KeyBinding &b : kKeys)
        if (keys[b.key]) pressed |= b.button;
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (keys[SDL_SCANCODE_LEFT]) lx = 0x00;
    if (keys[SDL_SCANCODE_RIGHT]) lx = 0xFF;
    if (keys[SDL_SCANCODE_UP]) ly = 0x00;
    if (keys[SDL_SCANCODE_DOWN]) ly = 0xFF;
    if (g_pad) {
        for (const PadBinding &b : kPadButtons)
            if (SDL_GameControllerGetButton(g_pad, b.button)) pressed |= b.mask;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) pressed |= kL2;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) pressed |= kR2;
        if (lx == 0x80) lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
        if (ly == 0x80) ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
        rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
    }
    static std::uint64_t last = ~0ull;
    const std::uint64_t state = pressed | (static_cast<std::uint64_t>(rx) << 16) | (static_cast<std::uint64_t>(ry) << 24) | (static_cast<std::uint64_t>(lx) << 32) |
                                (static_cast<std::uint64_t>(ly) << 40);
    if (state == last) return;
    last = state;
    ps2::set_pad_buttons(pressed);
    ps2::set_pad_axis(0, rx);
    ps2::set_pad_axis(1, ry);
    ps2::set_pad_axis(2, lx);
    ps2::set_pad_axis(3, ly);
}

void present() {
    if (!g_gpu_tried) {
        g_gpu_tried = true;
        g_gpu_present = attach_gpu();
        std::fprintf(stderr, "[window] %s\n", g_gpu_present ? "presenting through WebGPU" : "presenting through SDL");
        if (!g_gpu_present && !create_renderer()) ps2::halt("no renderer");
    }
    if (g_gpu_present) {
        ps2::present_frame();
        return;
    }
    if (!g_renderer) return;
    std::uint32_t w = 0, h = 0;
    if (!ps2::display_frame(g_pixels, w, h)) return;
    if (w != g_tex_w || h != g_tex_h) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(w), static_cast<int>(h));
        SDL_RenderSetLogicalSize(g_renderer, static_cast<int>(w), static_cast<int>(w) * 3 / 4);
        g_tex_w = w;
        g_tex_h = h;
    }
    SDL_UpdateTexture(g_texture, nullptr, g_pixels.data(), static_cast<int>(w * 4));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
}

constexpr Uint32 kAudioBytesPerMs = 48 * 4;

void pace() {
    if (g_audio) {
        // The audio device clock paces emulation: wait while more than ~80 ms of sound is queued.
        while (SDL_GetQueuedAudioSize(g_audio) > 80 * kAudioBytesPerMs) SDL_Delay(1);
        return;
    }
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t period = frequency / (ps2::g_options.pal ? 50 : 60);
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (g_last && now - g_last < period) {
        SDL_Delay(static_cast<Uint32>((period - (now - g_last)) * 1000 / frequency));
        g_last += period;
    } else {
        g_last = now;
    }
}

void show_speed() {
    static std::uint64_t start = 0, frames = 0;
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!start) start = now;
    ++frames;
    const double seconds = static_cast<double>(now - start) / static_cast<double>(SDL_GetPerformanceFrequency());
    if (seconds < 1.0) return;
    char title[128];
    const double queued = g_audio ? SDL_GetQueuedAudioSize(g_audio) / static_cast<double>(kAudioBytesPerMs) : 0.0;
    std::snprintf(title, sizeof title, "ps2-web-recomp  %.1f fps  audio %.0f ms", static_cast<double>(frames) / seconds, queued);
    SDL_SetWindowTitle(g_window, title);
    start = now;
    frames = 0;
}

void on_vblank() {
    show_speed();
    poll();
    present();
    pace();
}

void queue_audio(const std::int16_t *samples, std::size_t frames) {
    if (SDL_GetQueuedAudioSize(g_audio) > 500 * kAudioBytesPerMs) return;
    SDL_QueueAudio(g_audio, samples, static_cast<Uint32>(frames * 4));
}

}

bool open_window() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    g_window = SDL_CreateWindow("ps2-web-recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) {
        SDL_PauseAudioDevice(g_audio, 0);
        ps2::set_audio_sink(queue_audio);
    }
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; ++i)
        if (SDL_IsGameController(i)) g_pad = SDL_GameControllerOpen(i);
    ps2::g_options.on_vblank = on_vblank;
    return true;
}
