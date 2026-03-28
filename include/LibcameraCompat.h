/*
 * SurfCam Weather Pi — libcamera API differences (e.g. Debian bookworm 0.0.x vs Pi OS 0.2+).
 * SURFCAM_LIBCAMERA_LEGACY is set by CMake when pkg-config libcamera < 0.2.0.
 */
#pragma once

#if SURFCAM_LIBCAMERA_LEGACY

#include <cstddef>
#include <utility>

#include <libcamera/framebuffer.h>
#include <sys/mman.h>

namespace SurfCam {

/// mmap(2) RGB/MJPEG plane data (pre–plane.map() libcamera API).
class LegacyMappedPlane {
public:
    LegacyMappedPlane() = default;
    LegacyMappedPlane(const LegacyMappedPlane&) = delete;
    LegacyMappedPlane& operator=(const LegacyMappedPlane&) = delete;
    LegacyMappedPlane(LegacyMappedPlane&& o) noexcept { *this = std::move(o); }
    LegacyMappedPlane& operator=(LegacyMappedPlane&& o) noexcept {
        clear();
        addr_ = o.addr_;
        len_ = o.len_;
        o.addr_ = MAP_FAILED;
        o.len_ = 0;
        return *this;
    }

    bool mapRead(const libcamera::FrameBuffer* buffer) {
        clear();
        if (!buffer || buffer->planes().empty()) {
            return false;
        }
        const auto& p = buffer->planes()[0];
        len_ = p.length;
        addr_ = ::mmap(nullptr, len_, PROT_READ, MAP_SHARED, p.fd.get(), p.offset);
        return addr_ != MAP_FAILED;
    }

    void clear() {
        if (addr_ != MAP_FAILED) {
            ::munmap(addr_, len_);
            addr_ = MAP_FAILED;
            len_ = 0;
        }
    }

    ~LegacyMappedPlane() { clear(); }

    const void* data() const { return addr_ == MAP_FAILED ? nullptr : addr_; }
    std::size_t size() const { return len_; }
    explicit operator bool() const { return data() != nullptr; }

private:
    void* addr_{MAP_FAILED};
    std::size_t len_{0};
};

}  // namespace SurfCam

#endif  // SURFCAM_LIBCAMERA_LEGACY
