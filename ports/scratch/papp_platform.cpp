// The PAPP platform layer for Scratch Everywhere! (include/platforms/os.hpp,
// thread.hpp, timer.hpp): folders on the SD card, the loader's clock, and
// no threads (the runtime, its audio mixer included, runs on the app task).
#include "papp_port.h"

#include <os.hpp>
#include <thread.hpp>
#include <timer.hpp>

namespace OS {
bool toExit = false;
bool loadedSettings = false;
std::string *customProjectsPath = nullptr;
}  // namespace OS

bool OS::init()
{
    return true;
}

void OS::deinit()
{
}

std::string OS::getPlatform()
{
    return "ESP32-P4";
}

bool OS::isEnhancedPlatform()
{
    return false;
}

std::string OS::getFilesystemRootPrefix()
{
    return "";
}

std::string OS::getConfigFolderLocation()
{
    return PAPP_SCRATCH_DIR;
}

std::string OS::getScratchFolderLocation()
{
    const std::string custom = getCustomScratchFolderLocation();
    return custom.empty() ? std::string(PAPP_SCRATCH_DIR) : custom;
}

std::string OS::getRomFSLocation()
{
    return PAPP_SCRATCH_ROMFS;
}

bool OS::isOnline()
{
    return false;
}

bool OS::initWifi()
{
    return false;
}

void OS::deInitWifi()
{
}

std::string OS::getUsername()
{
    return "player";
}

// ── Threads ─────────────────────────────────────────────────────────────────
// None: the only callers (the threaded project loader, downloads, DECtalk)
// are compiled out or fall back to running inline when create() fails.

struct SE_Thread::Impl {
};

SE_Thread::SE_Thread() : impl(nullptr)
{
}

SE_Thread::~SE_Thread()
{
}

bool SE_Thread::create(void (*entryPoint)(void *), void *args, size_t stackSize, int prio, int coreID,
                       const std::string &name)
{
    (void)entryPoint;
    (void)args;
    (void)stackSize;
    (void)prio;
    (void)coreID;
    (void)name;
    return false;
}

void SE_Thread::join()
{
}

void SE_Thread::detach()
{
}

void SE_Thread::sleep(uint16_t milliseconds)
{
    papp_svc->delay_ms(milliseconds);
}

unsigned int SE_Thread::getCurrentThreadId()
{
    return 0;
}

// Everything runs on one task: nothing to lock.
struct SE_Mutex::Impl {
};

SE_Mutex::SE_Mutex() : impl(nullptr)
{
}

SE_Mutex::~SE_Mutex()
{
}

void SE_Mutex::init()
{
}

void SE_Mutex::lock()
{
}

void SE_Mutex::unlock()
{
}

bool SE_Mutex::tryLock()
{
    return true;
}

// ── Timer ───────────────────────────────────────────────────────────────────

Timer::Timer(const bool autoStart)
{
    startTime = 0;
    if (autoStart) {
        start();
    }
}

void Timer::start()
{
    startTime = (uint64_t)papp_time_us();
}

uint64_t Timer::getTimeMs()
{
    return ((uint64_t)papp_time_us() - startTime) / 1000;
}

double Timer::getTimeMsDouble()
{
    return ((uint64_t)papp_time_us() - startTime) / 1000.0;
}
