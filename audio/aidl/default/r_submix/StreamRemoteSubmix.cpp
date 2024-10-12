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

#define LOG_TAG "AHAL_StreamRemoteSubmix"
#include <android-base/logging.h>
#include <audio_utils/clock.h>
#include <error/Result.h>
#include <error/expected_utils.h>

#include "core-impl/StreamRemoteSubmix.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::hardware::audio::core::r_submix::SubmixRoute;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::MicrophoneDynamicInfo;
using aidl::android::media::audio::common::MicrophoneInfo;

namespace aidl::android::hardware::audio::core {

StreamRemoteSubmix::StreamRemoteSubmix(StreamContext* context, const Metadata& metadata)
    : StreamCommonImpl(context, metadata),
      mIsInput(isInput(metadata)),
      mStreamConfig{.sampleRate = context->getSampleRate(),
                    .format = context->getFormat(),
                    .channelLayout = context->getChannelLayout(),
                    .frameSize = context->getFrameSize()} {}

StreamRemoteSubmix::~StreamRemoteSubmix() {
    cleanupWorker();
}

::android::status_t StreamRemoteSubmix::init() {
    return ::android::OK;
}

::android::status_t StreamRemoteSubmix::drain(StreamDescriptor::DrainMode) {
    return ::android::OK;
}

::android::status_t StreamRemoteSubmix::flush() {
    return ::android::OK;
}

::android::status_t StreamRemoteSubmix::pause() {
    return ::android::OK;
}

::android::status_t StreamRemoteSubmix::standby() {
    if (mCurrentRoute) mCurrentRoute->standby(mIsInput);
    return ::android::OK;
}

::android::status_t StreamRemoteSubmix::start() {
    if (mDeviceAddressUpdated.load(std::memory_order_acquire)) {
        LOG(DEBUG) << __func__ << ": device address updated, reset current route";
        shutdown();
        mDeviceAddressUpdated.store(false, std::memory_order_release);
    }
    if (!mCurrentRoute) {
        RETURN_STATUS_IF_ERROR(setCurrentRoute());
        LOG(DEBUG) << __func__ << ": have current route? " << (mCurrentRoute != nullptr);
    }
    if (mCurrentRoute) mCurrentRoute->exitStandby(mIsInput);
    mStartTimeNs = ::android::uptimeNanos();
    mFramesSinceStart = 0;
    return ::android::OK;
}

// Remove references to the specified input and output streams.  When the device no longer
// references input and output streams destroy the associated pipe.
void StreamRemoteSubmix::shutdown() {
    if (!mCurrentRoute) return;
    mCurrentRoute->closeStream(mIsInput);
    // If all stream instances are closed, we can remove route information for this port.
    if (!mCurrentRoute->hasAtleastOneStreamOpen()) {
        mCurrentRoute->releasePipe();
        LOG(DEBUG) << __func__ << ": pipe destroyed";
        SubmixRoute::removeRoute(getDeviceAddress());
    }
    mCurrentRoute.reset();
}

::android::status_t StreamRemoteSubmix::transfer(void* buffer, size_t frameCount,
                                                 size_t* actualFrameCount, int32_t* latencyMs) {
    if (mDeviceAddressUpdated.load(std::memory_order_acquire)) {
        // 'setConnectedDevices' was called. I/O will be restarted.
        return ::android::OK;
    }

    *latencyMs = getDelayInUsForFrameCount(getStreamPipeSizeInFrames()) / 1000;
    LOG(VERBOSE) << __func__ << ": Latency " << *latencyMs << "ms";
    ::android::status_t status = ::android::OK;
    if (mCurrentRoute) {
        mCurrentRoute->exitStandby(mIsInput);
        status = mIsInput ? inRead(buffer, frameCount, actualFrameCount)
                          : outWrite(buffer, frameCount, actualFrameCount);
        if ((status != ::android::OK && mIsInput) ||
            ((status != ::android::OK && status != ::android::DEAD_OBJECT) && !mIsInput)) {
            return status;
        }
    } else {
        LOG(WARNING) << __func__ << ": no current route";
        if (mIsInput) {
            memset(buffer, 0, mStreamConfig.frameSize * frameCount);
        }
        *actualFrameCount = frameCount;
    }
    mFramesSinceStart += *actualFrameCount;
    // If there is no route, always block, otherwise:
    //  - Input streams always need to block, output streams need to block when there is no sink.
    //  - When the sink exists, more sophisticated blocking algorithm is implemented by MonoPipe.
    if (mCurrentRoute && !mIsInput && status != ::android::DEAD_OBJECT) return ::android::OK;
    const long bufferDurationUs =
            (*actualFrameCount) * MICROS_PER_SECOND / mContext.getSampleRate();
    const auto totalDurationUs = (::android::uptimeNanos() - mStartTimeNs) / NANOS_PER_MICROSECOND;
    const long totalOffsetUs =
            mFramesSinceStart * MICROS_PER_SECOND / mContext.getSampleRate() - totalDurationUs;
    LOG(VERBOSE) << __func__ << ": totalOffsetUs " << totalOffsetUs;
    if (totalOffsetUs > 0) {
        const long sleepTimeUs = std::min(totalOffsetUs, bufferDurationUs);
        LOG(VERBOSE) << __func__ << ": sleeping for " << sleepTimeUs << " us";
        usleep(sleepTimeUs);
    }
    return ::android::OK;
}

::android::status_t StreamRemoteSubmix::refinePosition(StreamDescriptor::Position* position) {
    if (!mCurrentRoute) {
        RETURN_STATUS_IF_ERROR(setCurrentRoute());
        if (!mCurrentRoute) return ::android::OK;
    }
    sp<MonoPipeReader> source = mCurrentRoute->getSource();
    if (source == nullptr) {
        return ::android::NO_INIT;
    }
    const ssize_t framesInPipe = source->availableToRead();
    if (framesInPipe <= 0) {
        // No need to update the position frames
        return ::android::OK;
    }
    if (mIsInput) {
        position->frames += framesInPipe;
    } else if (position->frames >= framesInPipe) {
        position->frames -= framesInPipe;
    }
    return ::android::OK;
}

long StreamRemoteSubmix::getDelayInUsForFrameCount(size_t frameCount) {
    return frameCount * MICROS_PER_SECOND / mStreamConfig.sampleRate;
}

// Calculate the maximum size of the pipe buffer in frames for the specified stream.
size_t StreamRemoteSubmix::getStreamPipeSizeInFrames() {
    if (!mCurrentRoute) return r_submix::kDefaultPipeSizeInFrames;
    auto pipeConfig = mCurrentRoute->getPipeConfig();
    const size_t maxFrameSize = std::max(mStreamConfig.frameSize, pipeConfig.frameSize);
    return (pipeConfig.frameCount * pipeConfig.frameSize) / maxFrameSize;
}

::android::status_t StreamRemoteSubmix::outWrite(void* buffer, size_t frameCount,
                                                 size_t* actualFrameCount) {
    sp<MonoPipe> sink = mCurrentRoute->getSink();
    if (sink != nullptr) {
        if (sink->isShutdown()) {
            sink.clear();
            if (++mWriteShutdownCount < kMaxErrorLogs) {
                LOG(DEBUG) << __func__ << ": pipe shutdown, ignoring the write. (limited logging)";
            }
            *actualFrameCount = frameCount;
            return ::android::DEAD_OBJECT;  // Induce wait in `transfer`.
        }
    } else {
        LOG(FATAL) << __func__ << ": without a pipe!";
        return ::android::UNKNOWN_ERROR;
    }
    mWriteShutdownCount = 0;

    LOG(VERBOSE) << __func__ << ": " << getDeviceAddress().toString() << ", " << frameCount
                 << " frames";

    const bool shouldBlockWrite = mCurrentRoute->shouldBlockWrite();
    size_t availableToWrite = sink->availableToWrite();
    // NOTE: sink has been checked above and sink and source life cycles are synchronized
    sp<MonoPipeReader> source = mCurrentRoute->getSource();
    // If the write to the sink should be blocked, flush enough frames from the pipe to make space
    // to write the most recent data.
    if (!shouldBlockWrite && availableToWrite < frameCount) {
        static uint8_t flushBuffer[64];
        const size_t flushBufferSizeFrames = sizeof(flushBuffer) / mStreamConfig.frameSize;
        size_t framesToFlushFromSource = frameCount - availableToWrite;
        LOG(DEBUG) << __func__ << ": flushing " << framesToFlushFromSource
                   << " frames from the pipe to avoid blocking";
        while (framesToFlushFromSource) {
            const size_t flushSize = std::min(framesToFlushFromSource, flushBufferSizeFrames);
            framesToFlushFromSource -= flushSize;
            // read does not block
            source->read(flushBuffer, flushSize);
        }
    }
    availableToWrite = sink->availableToWrite();

    if (!shouldBlockWrite && frameCount > availableToWrite) {
        LOG(WARNING) << __func__ << ": writing " << availableToWrite << " vs. requested "
                     << frameCount;
        // Truncate the request to avoid blocking.
        frameCount = availableToWrite;
    }
    ssize_t writtenFrames = sink->write(buffer, frameCount);
    if (writtenFrames < 0) {
        if (writtenFrames == (ssize_t)::android::NEGOTIATE) {
            LOG(ERROR) << __func__ << ": write to pipe returned NEGOTIATE";
            sink.clear();
            *actualFrameCount = 0;
            return ::android::UNKNOWN_ERROR;
        } else {
            // write() returned UNDERRUN or WOULD_BLOCK, retry
            LOG(ERROR) << __func__ << ": write to pipe returned unexpected " << writtenFrames;
            writtenFrames = sink->write(buffer, frameCount);
        }
    }

    if (writtenFrames < 0) {
        LOG(ERROR) << __func__ << ": failed writing to pipe with " << writtenFrames;
        *actualFrameCount = 0;
        return ::android::UNKNOWN_ERROR;
    }
    if (writtenFrames > 0 && frameCount > (size_t)writtenFrames) {
        LOG(WARNING) << __func__ << ": wrote " << writtenFrames << " vs. requested " << frameCount;
    }
    *actualFrameCount = writtenFrames;
    return ::android::OK;
}

::android::status_t StreamRemoteSubmix::inRead(void* buffer, size_t frameCount,
                                               size_t* actualFrameCount) {
    // in any case, it is emulated that data for the entire buffer was available
    memset(buffer, 0, mStreamConfig.frameSize * frameCount);
    *actualFrameCount = frameCount;

    // about to read from audio source
    sp<MonoPipeReader> source = mCurrentRoute->getSource();
    if (source == nullptr) {
        if (++mReadErrorCount < kMaxErrorLogs) {
            LOG(ERROR) << __func__
                       << ": no audio pipe yet we're trying to read! (not all errors will be "
                          "logged)";
        }
        return ::android::OK;
    }
    mReadErrorCount = 0;

    LOG(VERBOSE) << __func__ << ": " << getDeviceAddress().toString() << ", " << frameCount
                 << " frames";

    // read the data from the pipe
    char* buff = (char*)buffer;
    size_t actuallyRead = 0;
    long remainingFrames = frameCount;
    // Try to wait as long as possible for the audio duration, but leave some time for the call to
    // 'transfer' to complete. 'kReadAttemptSleepUs' is a good constant for this purpose because it
    // is by definition "strictly inferior" to the typical buffer duration.
    const long durationUs =
            std::max(0L, getDelayInUsForFrameCount(frameCount) - kReadAttemptSleepUs);
    const int64_t deadlineTimeNs = ::android::uptimeNanos() + durationUs * NANOS_PER_MICROSECOND;
    while (remainingFrames > 0) {
        ssize_t framesRead = source->read(buff, remainingFrames);
        LOG(VERBOSE) << __func__ << ": frames read " << framesRead;
        if (framesRead > 0) {
            remainingFrames -= framesRead;
            buff += framesRead * mStreamConfig.frameSize;
            LOG(VERBOSE) << __func__ << ": got " << framesRead
                         << " frames, remaining =" << remainingFrames;
            actuallyRead += framesRead;
        }
        if (::android::uptimeNanos() >= deadlineTimeNs) break;
        if (framesRead <= 0) {
            LOG(VERBOSE) << __func__ << ": read returned " << framesRead
                         << ", read failure, sleeping for " << kReadAttemptSleepUs << " us";
            usleep(kReadAttemptSleepUs);
        }
    }
    if (actuallyRead < frameCount) {
        if (++mReadFailureCount < kMaxReadFailureAttempts) {
            LOG(WARNING) << __func__ << ": read " << actuallyRead << " vs. requested " << frameCount
                         << " (not all errors will be logged)";
        }
    } else {
        mReadFailureCount = 0;
    }
    mCurrentRoute->updateReadCounterFrames(*actualFrameCount);
    return ::android::OK;
}

::android::status_t StreamRemoteSubmix::setCurrentRoute() {
    const auto address = getDeviceAddress();
    if (address == AudioDeviceAddress{}) {
        return ::android::OK;
    }
    mCurrentRoute = SubmixRoute::findOrCreateRoute(address, mStreamConfig);
    if (mCurrentRoute == nullptr) {
        return ::android::NO_INIT;
    }
    if (!mCurrentRoute->isStreamConfigValid(mIsInput, mStreamConfig)) {
        LOG(ERROR) << __func__ << ": invalid stream config";
        return ::android::NO_INIT;
    }
    sp<MonoPipe> sink = mCurrentRoute->getSink();
    if (sink == nullptr) {
        LOG(ERROR) << __func__ << ": nullptr sink when opening stream";
        return ::android::NO_INIT;
    }
    if ((!mIsInput || mCurrentRoute->isStreamInOpen()) && sink->isShutdown()) {
        LOG(DEBUG) << __func__ << ": Shut down sink when opening stream";
        if (::android::OK != mCurrentRoute->resetPipe()) {
            LOG(ERROR) << __func__ << ": reset pipe failed";
            return ::android::NO_INIT;
        }
    }
    mCurrentRoute->openStream(mIsInput);
    return ::android::OK;
}

ndk::ScopedAStatus StreamRemoteSubmix::prepareToClose() {
    if (!mIsInput) {
        const auto address = getDeviceAddress();
        if (address == AudioDeviceAddress{}) return ndk::ScopedAStatus::ok();
        std::shared_ptr<SubmixRoute> route = SubmixRoute::findRoute(address);
        if (route != nullptr) {
            sp<MonoPipe> sink = route->getSink();
            if (sink == nullptr) {
                ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
            }
            LOG(DEBUG) << __func__ << ": shutting down MonoPipe sink";

            sink->shutdown(true);
            // The client already considers this stream as closed, release the output end.
            route->closeStream(mIsInput);
        } else {
            LOG(DEBUG) << __func__ << ": stream already closed.";
            ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamRemoteSubmix::setConnectedDevices(const ConnectedDevices& devices) {
    if (devices.size() > 1) {
        LOG(ERROR) << __func__ << ": Only single device supported, got " << devices.size();
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    AudioDeviceAddress newAddress;
    if (!devices.empty()) {
        if (auto deviceDesc = devices.front().type;
            (mIsInput && deviceDesc.type != AudioDeviceType::IN_SUBMIX) ||
            (!mIsInput && deviceDesc.type != AudioDeviceType::OUT_SUBMIX)) {
            LOG(ERROR) << __func__ << ": Device type " << toString(deviceDesc.type)
                       << " not supported";
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
        newAddress = devices.front().address;
        LOG(DEBUG) << __func__ << ": connected to " << newAddress.toString();
    } else {
        LOG(DEBUG) << __func__ << ": disconnected";
    }
    RETURN_STATUS_IF_ERROR(StreamCommonImpl::setConnectedDevices(devices));
    std::lock_guard guard(mLock);
    if (mDeviceAddress != newAddress) {
        mDeviceAddress = newAddress;
        mDeviceAddressUpdated.store(true, std::memory_order_release);
    }
    return ndk::ScopedAStatus::ok();
}

StreamInRemoteSubmix::StreamInRemoteSubmix(StreamContext&& context,
                                           const SinkMetadata& sinkMetadata,
                                           const std::vector<MicrophoneInfo>& microphones)
    : StreamIn(std::move(context), microphones),
      StreamRemoteSubmix(&mContextInstance, sinkMetadata) {}

ndk::ScopedAStatus StreamInRemoteSubmix::getActiveMicrophones(
        std::vector<MicrophoneDynamicInfo>* _aidl_return) {
    LOG(DEBUG) << __func__ << ": not supported";
    *_aidl_return = std::vector<MicrophoneDynamicInfo>();
    return ndk::ScopedAStatus::ok();
}

StreamOutRemoteSubmix::StreamOutRemoteSubmix(StreamContext&& context,
                                             const SourceMetadata& sourceMetadata,
                                             const std::optional<AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamRemoteSubmix(&mContextInstance, sourceMetadata) {}

}  // namespace aidl::android::hardware::audio::core
