/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

#include <media/nbaio/MonoPipe.h>
#include <media/nbaio/MonoPipeReader.h>

#include "Stream.h"
#include "alsa/Utils.h"

namespace aidl::android::hardware::audio::core {

// This class is intended to be used as a base class for implementations
// that use TinyAlsa.
// This class does not define a complete stream implementation,
// and should never be used on its own. Derived classes are expected to
// provide necessary overrides for all interface methods omitted here.
class StreamAlsa : public StreamCommonImpl {
  public:
    StreamAlsa(StreamContext* context, const Metadata& metadata, int readWriteRetries);
    ~StreamAlsa();

    // Methods of 'DriverInterface'.
    ::android::status_t init(DriverCallbackInterface* callback) override;
    ::android::status_t drain(StreamDescriptor::DrainMode) override;
    ::android::status_t flush() override;
    ::android::status_t pause() override;
    ::android::status_t standby() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                 int32_t* latencyMs) override;
    ::android::status_t refinePosition(StreamDescriptor::Position* position) override;
    void shutdown() override;
    ndk::ScopedAStatus setGain(float gain) override;

  protected:
    // Called from 'start' to initialize 'mAlsaDeviceProxies', the vector must be non-empty.
    virtual std::vector<alsa::DeviceProfile> getDeviceProfiles() = 0;

    const size_t mBufferSizeFrames;
    const size_t mFrameSizeBytes;
    const int mSampleRate;
    const bool mIsInput;
    const std::optional<struct pcm_config> mConfig;
    const int mReadWriteRetries;

  private:
    ::android::NBAIO_Format getPipeFormat() const;
    ::android::sp<::android::MonoPipe> makeSink(bool writeCanBlock);
    ::android::sp<::android::MonoPipeReader> makeSource(::android::MonoPipe* pipe);
    void inputIoThread(size_t idx);
    void outputIoThread(size_t idx);
    void teardownIo();

    std::atomic<float> mGain = 1.0;

    // All fields below are only used on the worker thread.
    std::vector<alsa::DeviceProxy> mAlsaDeviceProxies;
    // Only 'libnbaio_mono' is vendor-accessible, thus no access to the multi-reader Pipe.
    std::vector<::android::sp<::android::MonoPipe>> mSinks;
    std::vector<::android::sp<::android::MonoPipeReader>> mSources;
    std::vector<std::thread> mIoThreads;
    std::atomic<bool> mIoThreadIsRunning = false;  // used by all threads
};

}  // namespace aidl::android::hardware::audio::core
