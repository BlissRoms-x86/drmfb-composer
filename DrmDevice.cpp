// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2019 Stephan Gerhold

#define LOG_TAG "drmfb-device"

#include <fcntl.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include "drm_unique_ptr.h"
#include "DrmDevice.h"

namespace android {
namespace hardware {
namespace graphics {
namespace composer {
namespace V2_1 {
namespace drmfb {

DrmDevice::DrmDevice(int fd) : mFd(fd), mHotplugThread(*this) {}
DrmDevice::DrmDevice(const std::string& path) : DrmDevice(open(path.c_str(), O_RDWR)) {
    if (mFd < 0)
        PLOG(ERROR) << "Failed to open DRM device (" << path << ")";
}
DrmDevice::DrmDevice()
    : DrmDevice(base::GetProperty("hwc.drm.device", "/dev/dri/card0")) {}

DrmDisplay* DrmDevice::getConnectedDisplay(uint32_t connector) {
    auto i = mDisplays.find(connector);
    if (i == mDisplays.end()) {
        return nullptr;
    }

    auto display = i->second.get();
    return display->connected() ? display : nullptr;
}

uint32_t DrmDevice::reserveCrtc(unsigned pipe) {
    auto mask = 1 << pipe;
    if (pipe < mCrtcs.size() && !(mUsedCrtcs & mask)) {
        mUsedCrtcs |= mask;
        return mCrtcs[pipe];
    }
    return 0;
}

void DrmDevice::freeCrtc(unsigned pipe) {
    if (pipe < mCrtcs.size()) {
        mUsedCrtcs &= ~(1 << pipe);
    }
}

bool DrmDevice::initialize() {
    if (mFd < 0)
        return false;

    drm::mode::unique_res_ptr res{drmModeGetResources(mFd)};
    if (!res) {
        PLOG(ERROR) << "Failed to get DRM mode resources";
        return false;
    }

    // Store the available CRTCs
    mCrtcs.assign(res->crtcs, res->crtcs + res->count_crtcs);

    // Create displays for each connector
    for (auto i = 0; i < res->count_connectors; ++i) {
        mDisplays.insert({res->connectors[i],
            std::make_unique<DrmDisplay>(*this, res->connectors[i])});
    }
    return true;
}

void DrmDevice::update() {
    // TODO: Add new (hotplug) connectors (mostly relevant for DP MST)
    for (auto& p : mDisplays) {
        p.second->update();
    }
}

void DrmDevice::enable(DrmCallback *callback) {
    update();
    mCallback = callback;

    // Attempt to report a "primary" (internal) display first
    auto primary = std::find_if(mDisplays.begin(), mDisplays.end(),
        [] (const auto& pair) {
            return pair.second->connected() && pair.second->internal();
        });
    if (primary != mDisplays.end()) {
        LOG(INFO) << "Reporting display " << *primary->second
            << " as primary display";
        primary->second->report();
        mPrimaryDisplay = primary->second->id();
    } else {
        for (auto i = mDisplays.begin(), end = mDisplays.end(); i != end; ++i) {
            if (i == primary)
                continue; // Primary display is already reported

            auto display = i->second.get();
            if (display->connected()) {
                display->report();
                // Report only one display during initialization on Oreo
                mPrimaryDisplay = display->id();
                break;
            }
        }
    }

    mHotplugThread.enable();
}

// Reports all other displays skipped during initialization
void DrmDevice::reportExternal() {
    if (!mPrimaryDisplay)
        return;

    for (auto i = mDisplays.begin(), end = mDisplays.end(); i != end; ++i) {
        auto display = i->second.get();
        if (display->id() != mPrimaryDisplay && display->connected()) {
            display->report();
        }
    }
    mPrimaryDisplay = 0;
}

void DrmDevice::disable() {
    mHotplugThread.disable();

    mCallback = nullptr;
    for (auto& p : mDisplays) {
        p.second->disable();
    }
}

}  // namespace drmfb
}  // namespace V2_1
}  // namespace composer
}  // namespace graphics
}  // namespace hardware
}  // namespace android
