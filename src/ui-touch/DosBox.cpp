// SPDX-License-Identifier: GPL-3.0-or-later
#include "DosBox.h"

#if defined(HAS_TDECK_GT911)

#include <Arduino.h>   // micros()

// Pull the Faux86 core in. Config.h must come first (it defines BUILD_STRING,
// which the other headers #error on). VM.h transitively includes the whole
// chipset, so this TU compiles the entire public core against wadamesh's flags.
#include "faux86/Config.h"
#include "faux86/Types.h"
#include "faux86/HostSystemInterface.h"
#include "faux86/Renderer.h"   // RenderSurface
#include "faux86/Video.h"      // Palette
#include "faux86/VM.h"

namespace Faux86 {
// The core requires the host to provide log(). SCAFFOLD: swallow it (Serial is
// owned by the companion link; the real build will route to an on-screen console).
void log(LogChannel, const char*, ...) {}
}

// NOTE: do NOT `using namespace Faux86` — wadamesh/MeshCore also declares a
// `HostSystemInterface`, so every Faux86 type is fully qualified below.
namespace {

// Minimal host — enough to compile the core's interface contract. Real behaviour
// (blit -> LVGL canvas / ST7789, PC speaker -> I2S, SD-backed DiskInterface)
// lands in step 2.
class DosFrameBuffer : public Faux86::FrameBufferInterface {
public:
  Faux86::RenderSurface* getSurface() override { return nullptr; }
  void setPalette(Faux86::Palette*) override {}
  void blit(uint16_t* /*px*/, int /*w*/, int /*h*/, int /*stride*/) override {}
};

class DosTimer : public Faux86::TimerInterface {
public:
  uint64_t getHostFreq() override { return 1000000ULL; }        // micros() base
  uint64_t getTicks() override { return (uint64_t)micros(); }
};

class DosAudio : public Faux86::AudioInterface {
public:
  void init(Faux86::VM&) override {}
  void shutdown() override {}
};

class DosHost : public Faux86::HostSystemInterface {
public:
  // init/resize are non-pure virtuals whose bodies live in Faux86's platform
  // frontends (win32/pi/linux — not vendored), so override them here.
  void init(Faux86::VM* inVM) override { vm = inVM; }
  void resize(uint32_t, uint32_t) override {}
  Faux86::FrameBufferInterface& getFrameBuffer() override { return fb; }
  Faux86::TimerInterface&       getTimer()       override { return timer; }
  Faux86::AudioInterface&       getAudio()       override { return audio; }
private:
  DosFrameBuffer fb;
  DosTimer       timer;
  DosAudio       audio;
};

DosHost s_host;

} // namespace

void DosBox::launch() {
  // SCAFFOLD: exercise the Faux86 API at compile time (a Config bound to our host
  // — trivial ctor, no allocation, no VM yet), then return. The real player
  // (VM + SD disks + render/input/audio bridges + paced simulate()) lands next.
  Faux86::Config cfg(&s_host);
  (void)cfg;
}

bool DosBox::isOpen()        { return false; }
void DosBox::steer(int, int) {}
void DosBox::keyChar(char)   {}
void DosBox::close()         {}

#else  // !HAS_TDECK_GT911 — no PSRAM budget / SD / input on the V4; stub it.

void DosBox::launch()        {}
bool DosBox::isOpen()        { return false; }
void DosBox::steer(int, int) {}
void DosBox::keyChar(char)   {}
void DosBox::close()         {}

#endif
