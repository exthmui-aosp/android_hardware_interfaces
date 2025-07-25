/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <pthread.h>

#define ATRACE_TAG ATRACE_TAG_AUDIO
#define LOG_TAG "AHAL_Stream"
#include <Utils.h>
#include <android-base/logging.h>
#include <android/binder_ibinder_platform.h>
#include <cutils/properties.h>
#include <utils/SystemClock.h>
#include <utils/Trace.h>

#include "core-impl/Stream.h"

using aidl::android::hardware::audio::common::AudioOffloadMetadata;
using aidl::android::hardware::audio::common::getChannelCount;
using aidl::android::hardware::audio::common::getFrameSizeInBytes;
using aidl::android::hardware::audio::common::hasMmapFlag;
using aidl::android::hardware::audio::common::isBitPositionFlagSet;
using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDualMonoMode;
using aidl::android::media::audio::common::AudioInputFlags;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioLatencyMode;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPlaybackRate;
using aidl::android::media::audio::common::MicrophoneDynamicInfo;
using aidl::android::media::audio::common::MicrophoneInfo;

namespace aidl::android::hardware::audio::core {

namespace {

template <typename MQTypeError>
auto fmqErrorHandler(const char* mqName) {
    return [m = std::string(mqName)](MQTypeError fmqError, std::string&& errorMessage) {
        CHECK_EQ(fmqError, MQTypeError::NONE) << m << ": " << errorMessage;
    };
}

}  // namespace

void StreamContext::fillDescriptor(StreamDescriptor* desc) {
    if (mCommandMQ) {
        desc->command = mCommandMQ->dupeDesc();
    }
    if (mReplyMQ) {
        desc->reply = mReplyMQ->dupeDesc();
    }
    desc->frameSizeBytes = getFrameSize();
    desc->bufferSizeFrames = getBufferSizeInFrames();
    if (mDataMQ) {
        desc->audio.set<StreamDescriptor::AudioBuffer::Tag::fmq>(mDataMQ->dupeDesc());
    } else {
        MmapBufferDescriptor mmapDesc;  // Move-only due to `fd`.
        mmapDesc.sharedMemory.fd = mMmapBufferDesc.sharedMemory.fd.dup();
        mmapDesc.sharedMemory.size = mMmapBufferDesc.sharedMemory.size;
        mmapDesc.burstSizeFrames = mMmapBufferDesc.burstSizeFrames;
        mmapDesc.flags = mMmapBufferDesc.flags;
        desc->audio.set<StreamDescriptor::AudioBuffer::Tag::mmap>(std::move(mmapDesc));
    }
}

size_t StreamContext::getBufferSizeInFrames() const {
    if (mDataMQ) {
        return mDataMQ->getQuantumCount() * mDataMQ->getQuantumSize() / getFrameSize();
    } else {
        return mMmapBufferDesc.sharedMemory.size / getFrameSize();
    }
}

size_t StreamContext::getFrameSize() const {
    return getFrameSizeInBytes(mFormat, mChannelLayout);
}

bool StreamContext::isValid() const {
    if (mCommandMQ && !mCommandMQ->isValid()) {
        LOG(ERROR) << "command FMQ is invalid";
        return false;
    }
    if (mReplyMQ && !mReplyMQ->isValid()) {
        LOG(ERROR) << "reply FMQ is invalid";
        return false;
    }
    if (getFrameSize() == 0) {
        LOG(ERROR) << "frame size is invalid";
        return false;
    }
    if (!isMmap() && mDataMQ && !mDataMQ->isValid()) {
        LOG(ERROR) << "data FMQ is invalid";
        return false;
    } else if (isMmap() &&
               (mMmapBufferDesc.sharedMemory.fd.get() == -1 ||
                mMmapBufferDesc.sharedMemory.size == 0 || mMmapBufferDesc.burstSizeFrames == 0)) {
        LOG(ERROR) << "mmap info is invalid" << mMmapBufferDesc.toString();
    }
    return true;
}

void StreamContext::startStreamDataProcessor() {
    auto streamDataProcessor = mStreamDataProcessor.lock();
    if (streamDataProcessor != nullptr) {
        streamDataProcessor->startDataProcessor(mSampleRate, getChannelCount(mChannelLayout),
                                                mFormat);
    }
}

void StreamContext::reset() {
    mCommandMQ.reset();
    mReplyMQ.reset();
    mDataMQ.reset();
    mMmapBufferDesc.sharedMemory.fd.set(-1);
}

pid_t StreamWorkerCommonLogic::getTid() const {
#if defined(__ANDROID__)
    return pthread_gettid_np(pthread_self());
#else
    return 0;
#endif
}

std::string StreamWorkerCommonLogic::init() {
    if (mContext->getCommandMQ() == nullptr) return "Command MQ is null";
    if (mContext->getReplyMQ() == nullptr) return "Reply MQ is null";
    if (!mContext->isMmap()) {
        StreamContext::DataMQ* const dataMQ = mContext->getDataMQ();
        if (dataMQ == nullptr) return "Data MQ is null";
        if (sizeof(DataBufferElement) != dataMQ->getQuantumSize()) {
            return "Unexpected Data MQ quantum size: " + std::to_string(dataMQ->getQuantumSize());
        }
        mDataBufferSize = dataMQ->getQuantumCount() * dataMQ->getQuantumSize();
        mDataBuffer.reset(new (std::nothrow) DataBufferElement[mDataBufferSize]);
        if (mDataBuffer == nullptr) {
            return "Failed to allocate data buffer for element count " +
                   std::to_string(dataMQ->getQuantumCount()) +
                   ", size in bytes: " + std::to_string(mDataBufferSize);
        }
    }
    if (::android::status_t status = mDriver->init(this /*DriverCallbackInterface*/);
        status != STATUS_OK) {
        return "Failed to initialize the driver: " + std::to_string(status);
    }
    return "";
}

void StreamWorkerCommonLogic::onBufferStateChange(size_t /*bufferFramesLeft*/) {}
void StreamWorkerCommonLogic::onClipStateChange(size_t /*clipFramesLeft*/, bool /*hasNextClip*/) {}

void StreamWorkerCommonLogic::populateReply(StreamDescriptor::Reply* reply,
                                            bool isConnected) const {
    static const StreamDescriptor::Position kUnknownPosition = {
            .frames = StreamDescriptor::Position::UNKNOWN,
            .timeNs = StreamDescriptor::Position::UNKNOWN};
    reply->status = STATUS_OK;
    if (isConnected) {
        reply->observable.frames = mContext->getFrameCount();
        reply->observable.timeNs = ::android::uptimeNanos();
        if (auto status = mDriver->refinePosition(&reply->observable); status != ::android::OK) {
            reply->observable = kUnknownPosition;
        }
    } else {
        reply->observable = reply->hardware = kUnknownPosition;
    }
    if (mContext->isMmap()) {
        if (auto status = mDriver->getMmapPositionAndLatency(&reply->hardware, &reply->latencyMs);
            status != ::android::OK) {
            reply->hardware = kUnknownPosition;
            reply->latencyMs = StreamDescriptor::LATENCY_UNKNOWN;
        }
    }
}

void StreamWorkerCommonLogic::populateReplyWrongState(
        StreamDescriptor::Reply* reply, const StreamDescriptor::Command& command) const {
    LOG(WARNING) << "command '" << toString(command.getTag())
                 << "' can not be handled in the state " << toString(mState);
    reply->status = STATUS_INVALID_OPERATION;
}

const std::string StreamInWorkerLogic::kThreadName = "reader";

StreamInWorkerLogic::Status StreamInWorkerLogic::cycle() {
    // Note: for input streams, draining is driven by the client, thus
    // "empty buffer" condition can only happen while handling the 'burst'
    // command. Thus, unlike for output streams, it does not make sense to
    // delay the 'DRAINING' state here by 'mTransientStateDelayMs'.
    // TODO: Add a delay for transitions of async operations when/if they added.

    StreamDescriptor::Command command{};
    if (!mContext->getCommandMQ()->readBlocking(&command, 1)) {
        LOG(ERROR) << __func__ << ": reading of command from MQ failed";
        mState = StreamDescriptor::State::ERROR;
        return Status::ABORT;
    }
    using Tag = StreamDescriptor::Command::Tag;
    using LogSeverity = ::android::base::LogSeverity;
    const LogSeverity severity =
            command.getTag() == Tag::burst || command.getTag() == Tag::getStatus
                    ? LogSeverity::VERBOSE
                    : LogSeverity::DEBUG;
    LOG(severity) << __func__ << ": received command " << command.toString() << " in "
                  << kThreadName;
    StreamDescriptor::Reply reply{};
    reply.status = STATUS_BAD_VALUE;
    switch (command.getTag()) {
        case Tag::halReservedExit: {
            const int32_t cookie = command.get<Tag::halReservedExit>();
            StreamInWorkerLogic::Status status = Status::CONTINUE;
            if (cookie == (mContext->getInternalCommandCookie() ^ getTid())) {
                mDriver->shutdown();
                setClosed();
                status = Status::EXIT;
            } else {
                LOG(WARNING) << __func__ << ": EXIT command has a bad cookie: " << cookie;
            }
            if (cookie != 0) {  // This is an internal command, no need to reply.
                return status;
            }
            // `cookie == 0` can only occur in the context of a VTS test, need to reply.
            break;
        }
        case Tag::getStatus:
            populateReply(&reply, mIsConnected);
            break;
        case Tag::start:
            if (mState == StreamDescriptor::State::STANDBY ||
                mState == StreamDescriptor::State::DRAINING) {
                if (::android::status_t status = mDriver->start(); status == ::android::OK) {
                    populateReply(&reply, mIsConnected);
                    mState = mState == StreamDescriptor::State::STANDBY
                                     ? StreamDescriptor::State::IDLE
                                     : StreamDescriptor::State::ACTIVE;
                } else {
                    LOG(ERROR) << __func__ << ": start failed: " << status;
                    mState = StreamDescriptor::State::ERROR;
                }
            } else {
                populateReplyWrongState(&reply, command);
            }
            break;
        case Tag::burst:
            if (const int32_t fmqByteCount = command.get<Tag::burst>(); fmqByteCount >= 0) {
                LOG(VERBOSE) << __func__ << ": '" << toString(command.getTag()) << "' command for "
                             << fmqByteCount << " bytes";
                if (mState == StreamDescriptor::State::IDLE ||
                    mState == StreamDescriptor::State::ACTIVE ||
                    mState == StreamDescriptor::State::PAUSED ||
                    mState == StreamDescriptor::State::DRAINING) {
                    if (bool success =
                                mContext->isMmap() ? readMmap(&reply) : read(fmqByteCount, &reply);
                        !success) {
                        mState = StreamDescriptor::State::ERROR;
                    }
                    if (mState == StreamDescriptor::State::IDLE ||
                        mState == StreamDescriptor::State::PAUSED) {
                        mState = StreamDescriptor::State::ACTIVE;
                    } else if (mState == StreamDescriptor::State::DRAINING) {
                        // To simplify the reference code, we assume that the read operation
                        // has consumed all the data remaining in the hardware buffer.
                        // In a real implementation, here we would either remain in
                        // the 'DRAINING' state, or transfer to 'STANDBY' depending on the
                        // buffer state.
                        mState = StreamDescriptor::State::STANDBY;
                    }
                } else {
                    populateReplyWrongState(&reply, command);
                }
            } else {
                LOG(WARNING) << __func__ << ": invalid burst byte count: " << fmqByteCount;
            }
            break;
        case Tag::drain:
            if (const auto mode = command.get<Tag::drain>();
                mode == StreamDescriptor::DrainMode::DRAIN_UNSPECIFIED) {
                if (mState == StreamDescriptor::State::ACTIVE) {
                    if (::android::status_t status = mDriver->drain(mode);
                        status == ::android::OK) {
                        populateReply(&reply, mIsConnected);
                        mState = StreamDescriptor::State::DRAINING;
                    } else {
                        LOG(ERROR) << __func__ << ": drain failed: " << status;
                        mState = StreamDescriptor::State::ERROR;
                    }
                } else {
                    populateReplyWrongState(&reply, command);
                }
            } else {
                LOG(WARNING) << __func__ << ": invalid drain mode: " << toString(mode);
            }
            break;
        case Tag::standby:
            if (mState == StreamDescriptor::State::IDLE) {
                populateReply(&reply, mIsConnected);
                if (::android::status_t status = mDriver->standby(); status == ::android::OK) {
                    mState = StreamDescriptor::State::STANDBY;
                } else {
                    LOG(ERROR) << __func__ << ": standby failed: " << status;
                    mState = StreamDescriptor::State::ERROR;
                }
            } else {
                populateReplyWrongState(&reply, command);
            }
            break;
        case Tag::pause:
            if (mState == StreamDescriptor::State::ACTIVE) {
                if (::android::status_t status = mDriver->pause(); status == ::android::OK) {
                    populateReply(&reply, mIsConnected);
                    mState = StreamDescriptor::State::PAUSED;
                } else {
                    LOG(ERROR) << __func__ << ": pause failed: " << status;
                    mState = StreamDescriptor::State::ERROR;
                }
            } else {
                populateReplyWrongState(&reply, command);
            }
            break;
        case Tag::flush:
            if (mState == StreamDescriptor::State::PAUSED) {
                if (::android::status_t status = mDriver->flush(); status == ::android::OK) {
                    populateReply(&reply, mIsConnected);
                    mState = StreamDescriptor::State::STANDBY;
                } else {
                    LOG(ERROR) << __func__ << ": flush failed: " << status;
                    mState = StreamDescriptor::State::ERROR;
                }
            } else {
                populateReplyWrongState(&reply, command);
            }
            break;
    }
    reply.state = mState;
    LOG(severity) << __func__ << ": writing reply " << reply.toString();
    if (!mContext->getReplyMQ()->writeBlocking(&reply, 1)) {
        LOG(ERROR) << __func__ << ": writing of reply " << reply.toString() << " to MQ failed";
        mState = StreamDescriptor::State::ERROR;
        return Status::ABORT;
    }
    return Status::CONTINUE;
}

bool StreamInWorkerLogic::read(size_t clientSize, StreamDescriptor::Reply* reply) {
    ATRACE_CALL();
    StreamContext::DataMQ* const dataMQ = mContext->getDataMQ();
    const size_t byteCount = std::min({clientSize, dataMQ->availableToWrite(), mDataBufferSize});
    const bool isConnected = mIsConnected;
    const size_t frameSize = mContext->getFrameSize();
    size_t actualFrameCount = 0;
    bool fatal = false;
    int32_t latency = mContext->getNominalLatencyMs();
    if (isConnected) {
        if (::android::status_t status = mDriver->transfer(mDataBuffer.get(), byteCount / frameSize,
                                                           &actualFrameCount, &latency);
            status != ::android::OK) {
            fatal = true;
            LOG(ERROR) << __func__ << ": read failed: " << status;
        }
    } else {
        usleep(3000);  // Simulate blocking transfer delay.
        for (size_t i = 0; i < byteCount; ++i) mDataBuffer[i] = 0;
        actualFrameCount = byteCount / frameSize;
    }
    const size_t actualByteCount = actualFrameCount * frameSize;
    if (bool success = actualByteCount > 0 ? dataMQ->write(&mDataBuffer[0], actualByteCount) : true;
        success) {
        LOG(VERBOSE) << __func__ << ": writing of " << actualByteCount << " bytes into data MQ"
                     << " succeeded; connected? " << isConnected;
        // Frames are provided and counted regardless of connection status.
        reply->fmqByteCount += actualByteCount;
        mContext->advanceFrameCount(actualFrameCount);
        populateReply(reply, isConnected);
    } else {
        LOG(WARNING) << __func__ << ": writing of " << actualByteCount
                     << " bytes of data to MQ failed";
        reply->status = STATUS_NOT_ENOUGH_DATA;
    }
    reply->latencyMs = latency;
    return !fatal;
}

bool StreamInWorkerLogic::readMmap(StreamDescriptor::Reply* reply) {
    void* buffer = nullptr;
    size_t frameCount = 0;
    size_t actualFrameCount = 0;
    int32_t latency = mContext->getNominalLatencyMs();
    // use default-initialized parameter values for mmap stream.
    if (::android::status_t status =
                mDriver->transfer(buffer, frameCount, &actualFrameCount, &latency);
        status == ::android::OK) {
        populateReply(reply, mIsConnected);
        reply->latencyMs = latency;
        return true;
    } else {
        LOG(ERROR) << __func__ << ": transfer failed: " << status;
        return false;
    }
}

const std::string StreamOutWorkerLogic::kThreadName = "writer";

void StreamOutWorkerLogic::onBufferStateChange(size_t bufferFramesLeft) {
    const StreamDescriptor::State state = mState;
    const DrainState drainState = mDrainState;
    LOG(DEBUG) << __func__ << ": state: " << toString(state) << ", drainState: " << drainState
               << ", bufferFramesLeft: " << bufferFramesLeft;
    if (state == StreamDescriptor::State::TRANSFERRING || drainState == DrainState::EN_SENT) {
        if (state == StreamDescriptor::State::TRANSFERRING) {
            mState = StreamDescriptor::State::ACTIVE;
        }
        std::shared_ptr<IStreamCallback> asyncCallback = mContext->getAsyncCallback();
        if (asyncCallback != nullptr) {
            LOG(VERBOSE) << __func__ << ": sending onTransferReady";
            ndk::ScopedAStatus status = asyncCallback->onTransferReady();
            if (!status.isOk()) {
                LOG(ERROR) << __func__ << ": error from onTransferReady: " << status;
            }
        }
    }
}

void StreamOutWorkerLogic::onClipStateChange(size_t clipFramesLeft, bool hasNextClip) {
    const DrainState drainState = mDrainState;
    std::shared_ptr<IStreamCallback> asyncCallback = mContext->getAsyncCallback();
    LOG(DEBUG) << __func__ << ": drainState: " << drainState << "; clipFramesLeft "
               << clipFramesLeft << "; hasNextClip? " << hasNextClip << "; asyncCallback? "
               << (asyncCallback != nullptr);
    if (drainState != DrainState::NONE && clipFramesLeft == 0) {
        mState =
                hasNextClip ? StreamDescriptor::State::TRANSFERRING : StreamDescriptor::State::IDLE;
        mDrainState = DrainState::NONE;
        if ((drainState == DrainState::ALL || drainState == DrainState::EN_SENT) &&
            asyncCallback != nullptr) {
            LOG(DEBUG) << __func__ << ": sending onDrainReady";
            // For EN_SENT, this is the second onDrainReady which notifies about clip transition.
            ndk::ScopedAStatus status = asyncCallback->onDrainReady();
            if (!status.isOk()) {
                LOG(ERROR) << __func__ << ": error from onDrainReady: " << status;
            }
        }
    } else if (drainState == DrainState::EN && clipFramesLeft > 0) {
        // The stream state does not change, it is still draining.
        mDrainState = DrainState::EN_SENT;
        if (asyncCallback != nullptr) {
            LOG(DEBUG) << __func__ << ": sending onDrainReady";
            ndk::ScopedAStatus status = asyncCallback->onDrainReady();
            if (!status.isOk()) {
                LOG(ERROR) << __func__ << ": error from onDrainReady: " << status;
            }
        }
    }
}

StreamOutWorkerLogic::Status StreamOutWorkerLogic::cycle() {
    // Non-blocking mode is handled within 'onClipStateChange'
    if (std::shared_ptr<IStreamCallback> asyncCallback = mContext->getAsyncCallback();
        mState == StreamDescriptor::State::DRAINING && asyncCallback == nullptr) {
        if (auto stateDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - mTransientStateStart);
            stateDurationMs >= mTransientStateDelayMs) {
            mState = StreamDescriptor::State::IDLE;
            if (mTransientStateDelayMs.count() != 0) {
                LOG(DEBUG) << __func__ << ": switched to state " << toString(mState)
                           << " after a timeout";
            }
        }
    }

    StreamDescriptor::Command command{};
    if (!mContext->getCommandMQ()->readBlocking(&command, 1)) {
        LOG(ERROR) << __func__ << ": reading of command from MQ failed";
        mState = StreamDescriptor::State::ERROR;
        return Status::ABORT;
    }
    using Tag = StreamDescriptor::Command::Tag;
    using LogSeverity = ::android::base::LogSeverity;
    const LogSeverity severity =
            command.getTag() == Tag::burst || command.getTag() == Tag::getStatus
                    ? LogSeverity::VERBOSE
                    : LogSeverity::DEBUG;
    LOG(severity) << __func__ << ": received command " << command.toString() << " in "
                  << kThreadName;
    StreamDescriptor::Reply reply{};
    reply.status = STATUS_BAD_VALUE;
    using Tag = StreamDescriptor::Command::Tag;
    switch (command.getTag()) {
        case Tag::halReservedExit: {
            const int32_t cookie = command.get<Tag::halReservedExit>();
            StreamOutWorkerLogic::Status status = Status::CONTINUE;
            if (cookie == (mContext->getInternalCommandCookie() ^ getTid())) {
                mDriver->shutdown();
                setClosed();
                status = Status::EXIT;
            } else {
                LOG(WARNING) << __func__ << ": EXIT command has a bad cookie: " << cookie;
            }
            if (cookie != 0) {  // This is an internal command, no need to reply.
                return status;
            }
            // `cookie == 0` can only occur in the context of a VTS test, need to reply.
            break;
        }
        case Tag::getStatus:
            populateReply(&reply, mIsConnected);
            break;
        case Tag::start: {
            std::optional<StreamDescriptor::State> nextState;
            switch (mState) {
                case StreamDescriptor::State::STANDBY:
                    nextState = StreamDescriptor::State::IDLE;
                    break;
                case StreamDescriptor::State::PAUSED:
                    nextState = StreamDescriptor::State::ACTIVE;
                    break;
                case StreamDescriptor::State::DRAIN_PAUSED:
                    nextState = StreamDescriptor::State::DRAINING;
                    break;
                case StreamDescriptor::State::TRANSFER_PAUSED:
                    nextState = StreamDescriptor::State::TRANSFERRING;
                    break;
                default:
                    populateReplyWrongState(&reply, command);
            }
            if (nextState.has_value()) {
                if (::android::status_t status = mDriver->start(); status == ::android::OK) {
                    populateReply(&reply, mIsConnected);
                    if (*nextState == StreamDescriptor::State::IDLE ||
                        *nextState == StreamDescriptor::State::ACTIVE) {
                        mState = *nextState;
                    } else {
                        switchToTransientState(*nextState);
                    }
                } else {
                    LOG(ERROR) << __func__ << ": start failed: " << status;
                    mState = StreamDescriptor::State::ERROR;
                }
            }
        } break;
        case Tag::burst:
            if (const int32_t fmqByteCount = command.get<Tag::burst>(); fmqByteCount >= 0) {
                LOG(VERBOSE) << __func__ << ": '" << toString(command.getTag()) << "' command for "
                             << fmqByteCount << " bytes";
                if (mState != StreamDescriptor::State::ERROR &&
                    mState != StreamDescriptor::State::TRANSFERRING &&
                    mState != StreamDescriptor::State::TRANSFER_PAUSED) {
                    if (bool success = mContext->isMmap() ? writeMmap(&reply)
                                                          : write(fmqByteCount, &reply);
                        !success) {
                        mState = StreamDescriptor::State::ERROR;
                    }
                    std::shared_ptr<IStreamCallback> asyncCallback = mContext->getAsyncCallback();
                    if (mState == StreamDescriptor::State::STANDBY ||
                        mState == StreamDescriptor::State::DRAIN_PAUSED ||
                        mState == StreamDescriptor::State::PAUSED) {
                        if (asyncCallback == nullptr ||
                            mState != StreamDescriptor::State::DRAIN_PAUSED) {
                            mState = StreamDescriptor::State::PAUSED;
                        } else {
                            mState = StreamDescriptor::State::TRANSFER_PAUSED;
                        }
                    } else if (mState == StreamDescriptor::State::IDLE ||
                               mState == StreamDescriptor::State::ACTIVE ||
                               (mState == StreamDescriptor::State::DRAINING &&
                                mDrainState != DrainState::EN_SENT)) {
                        if (asyncCallback == nullptr || reply.fmqByteCount == fmqByteCount) {
                            mState = StreamDescriptor::State::ACTIVE;
                        } else {
                            switchToTransientState(StreamDescriptor::State::TRANSFERRING);
                        }
                    } else if (mState == StreamDescriptor::State::DRAINING &&
                               mDrainState == DrainState::EN_SENT) {
                        // keep mState
                    }
                } else {
                    populateReplyWrongState(&reply, command);
                }
            } else {
                LOG(WARNING) << __func__ << ": invalid burst byte count: " << fmqByteCount;
            }
            break;
        case Tag::drain:
            if (const auto mode = command.get<Tag::drain>();
                mode == StreamDescriptor::DrainMode::DRAIN_ALL ||
                mode == StreamDescriptor::DrainMode::DRAIN_EARLY_NOTIFY) {
                if (mState == StreamDescriptor::State::ACTIVE ||
                    mState == StreamDescriptor::State::TRANSFERRING) {
                    if (::android::status_t status = mDriver->drain(mode);
                        status == ::android::OK) {
                        populateReply(&reply, mIsConnected);
                        if (mState == StreamDescriptor::State::ACTIVE &&
                            mContext->getForceSynchronousDrain()) {
                            mState = StreamDescriptor::State::IDLE;
                        } else {
                            switchToTransientState(StreamDescriptor::State::DRAINING);
                            mDrainState = mode == StreamDescriptor::DrainMode::DRAIN_EARLY_NOTIFY
                                                  ? DrainState::EN
                                                  : DrainState::ALL;
                        }
                    } else {
                        LOG(ERROR) << __func__ << ": drain failed: " << status;
                        mState = StreamDescriptor::State::ERROR;
                    }
                } else if (mState == StreamDescriptor::State::TRANSFER_PAUSED) {
                    mState = StreamDescriptor::State::DRAIN_PAUSED;
                    populateReply(&reply, mIsConnected);
                } else {
                    populateReplyWrongState(&reply, command);
                }
            } else {
                LOG(WARNING) << __func__ << ": invalid drain mode: " << toString(mode);
            }
            break;
        case Tag::standby:
            if (mState == StreamDescriptor::State::IDLE) {
                populateReply(&reply, mIsConnected);
                if (::android::status_t status = mDriver->standby(); status == ::android::OK) {
                    mState = StreamDescriptor::State::STANDBY;
                } else {
                    LOG(ERROR) << __func__ << ": standby failed: " << status;
                    mState = StreamDescriptor::State::ERROR;
                }
            } else {
                populateReplyWrongState(&reply, command);
            }
            break;
        case Tag::pause: {
            std::optional<StreamDescriptor::State> nextState;
            switch (mState) {
                case StreamDescriptor::State::ACTIVE:
                    nextState = StreamDescriptor::State::PAUSED;
                    break;
                case StreamDescriptor::State::DRAINING:
                    nextState = StreamDescriptor::State::DRAIN_PAUSED;
                    break;
                case StreamDescriptor::State::TRANSFERRING:
                    nextState = StreamDescriptor::State::TRANSFER_PAUSED;
                    break;
                default:
                    populateReplyWrongState(&reply, command);
            }
            if (nextState.has_value()) {
                if (::android::status_t status = mDriver->pause(); status == ::android::OK) {
                    populateReply(&reply, mIsConnected);
                    mState = nextState.value();
                } else {
                    LOG(ERROR) << __func__ << ": pause failed: " << status;
                    mState = StreamDescriptor::State::ERROR;
                }
            }
        } break;
        case Tag::flush:
            if (mState == StreamDescriptor::State::PAUSED ||
                mState == StreamDescriptor::State::DRAIN_PAUSED ||
                mState == StreamDescriptor::State::TRANSFER_PAUSED) {
                if (::android::status_t status = mDriver->flush(); status == ::android::OK) {
                    populateReply(&reply, mIsConnected);
                    mState = StreamDescriptor::State::IDLE;
                } else {
                    LOG(ERROR) << __func__ << ": flush failed: " << status;
                    mState = StreamDescriptor::State::ERROR;
                }
            } else {
                populateReplyWrongState(&reply, command);
            }
            break;
    }
    reply.state = mState;
    LOG(severity) << __func__ << ": writing reply " << reply.toString();
    if (!mContext->getReplyMQ()->writeBlocking(&reply, 1)) {
        LOG(ERROR) << __func__ << ": writing of reply " << reply.toString() << " to MQ failed";
        mState = StreamDescriptor::State::ERROR;
        return Status::ABORT;
    }
    return Status::CONTINUE;
}

bool StreamOutWorkerLogic::write(size_t clientSize, StreamDescriptor::Reply* reply) {
    ATRACE_CALL();
    StreamContext::DataMQ* const dataMQ = mContext->getDataMQ();
    const size_t readByteCount = dataMQ->availableToRead();
    const size_t frameSize = mContext->getFrameSize();
    bool fatal = false;
    int32_t latency = mContext->getNominalLatencyMs();
    if (readByteCount > 0 ? dataMQ->read(&mDataBuffer[0], readByteCount) : true) {
        const bool isConnected = mIsConnected;
        LOG(VERBOSE) << __func__ << ": reading of " << readByteCount << " bytes from data MQ"
                     << " succeeded; connected? " << isConnected;
        // Amount of data that the HAL module is going to actually use.
        size_t byteCount = std::min({clientSize, readByteCount, mDataBufferSize});
        if (byteCount >= frameSize && mContext->getForceTransientBurst()) {
            // In order to prevent the state machine from going to ACTIVE state,
            // simulate partial write.
            byteCount -= frameSize;
        }
        size_t actualFrameCount = 0;
        if (isConnected) {
            if (::android::status_t status = mDriver->transfer(
                        mDataBuffer.get(), byteCount / frameSize, &actualFrameCount, &latency);
                status != ::android::OK) {
                fatal = true;
                LOG(ERROR) << __func__ << ": write failed: " << status;
            }
            auto streamDataProcessor = mContext->getStreamDataProcessor().lock();
            if (streamDataProcessor != nullptr) {
                streamDataProcessor->process(mDataBuffer.get(), actualFrameCount * frameSize);
            }
        } else {
            if (mContext->getAsyncCallback() == nullptr) {
                usleep(3000);  // Simulate blocking transfer delay.
            }
            actualFrameCount = byteCount / frameSize;
        }
        const size_t actualByteCount = actualFrameCount * frameSize;
        // Frames are consumed and counted regardless of the connection status.
        reply->fmqByteCount += actualByteCount;
        mContext->advanceFrameCount(actualFrameCount);
        populateReply(reply, isConnected);
    } else {
        LOG(WARNING) << __func__ << ": reading of " << readByteCount
                     << " bytes of data from MQ failed";
        reply->status = STATUS_NOT_ENOUGH_DATA;
    }
    reply->latencyMs = latency;
    return !fatal;
}

bool StreamOutWorkerLogic::writeMmap(StreamDescriptor::Reply* reply) {
    void* buffer = nullptr;
    size_t frameCount = 0;
    size_t actualFrameCount = 0;
    int32_t latency = mContext->getNominalLatencyMs();
    // use default-initialized parameter values for mmap stream.
    if (::android::status_t status =
                mDriver->transfer(buffer, frameCount, &actualFrameCount, &latency);
        status == ::android::OK) {
        populateReply(reply, mIsConnected);
        reply->latencyMs = latency;
        return true;
    } else {
        LOG(ERROR) << __func__ << ": transfer failed: " << status;
        return false;
    }
}

StreamCommonImpl::~StreamCommonImpl() {
    // It is responsibility of the class that implements 'DriverInterface' to call 'cleanupWorker'
    // in the destructor. Note that 'cleanupWorker' can not be properly called from this destructor
    // because any subclasses have already been destroyed and thus the 'DriverInterface'
    // implementation is not valid. Thus, here it can only be asserted whether the subclass has done
    // its job.
    if (!mWorkerStopIssued && !isClosed()) {
        LOG(FATAL) << __func__ << ": the stream implementation must call 'cleanupWorker' "
                   << "in order to clean up the worker thread.";
    }
}

ndk::ScopedAStatus StreamCommonImpl::initInstance(
        const std::shared_ptr<StreamCommonInterface>& delegate) {
    mCommon = ndk::SharedRefBase::make<StreamCommonDelegator>(delegate);
    if (!mWorker->start()) {
        LOG(ERROR) << __func__ << ": Worker start error: " << mWorker->getError();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    setWorkerThreadPriority(mWorker->getTid());
    getContext().getCommandMQ()->setErrorHandler(
            fmqErrorHandler<StreamContext::CommandMQ::Error>("CommandMQ"));
    getContext().getReplyMQ()->setErrorHandler(
            fmqErrorHandler<StreamContext::ReplyMQ::Error>("ReplyMQ"));
    if (getContext().getDataMQ() != nullptr) {
        getContext().getDataMQ()->setErrorHandler(
                fmqErrorHandler<StreamContext::DataMQ::Error>("DataMQ"));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamCommonImpl::getStreamCommonCommon(
        std::shared_ptr<IStreamCommon>* _aidl_return) {
    if (!mCommon) {
        LOG(FATAL) << __func__ << ": the common interface was not created";
    }
    *_aidl_return = mCommon.getInstance();
    LOG(DEBUG) << __func__ << ": returning " << _aidl_return->get()->asBinder().get();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamCommonImpl::updateHwAvSyncId(int32_t in_hwAvSyncId) {
    LOG(DEBUG) << __func__ << ": id " << in_hwAvSyncId;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamCommonImpl::getVendorParameters(
        const std::vector<std::string>& in_ids, std::vector<VendorParameter>* _aidl_return) {
    LOG(DEBUG) << __func__ << ": id count: " << in_ids.size();
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamCommonImpl::setVendorParameters(
        const std::vector<VendorParameter>& in_parameters, bool in_async) {
    LOG(DEBUG) << __func__ << ": parameters count " << in_parameters.size()
               << ", async: " << in_async;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamCommonImpl::addEffect(
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    if (in_effect == nullptr) {
        LOG(DEBUG) << __func__ << ": null effect";
    } else {
        LOG(DEBUG) << __func__ << ": effect Binder" << in_effect->asBinder().get();
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamCommonImpl::removeEffect(
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    if (in_effect == nullptr) {
        LOG(DEBUG) << __func__ << ": null effect";
    } else {
        LOG(DEBUG) << __func__ << ": effect Binder" << in_effect->asBinder().get();
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamCommonImpl::close() {
    LOG(DEBUG) << __func__;
    if (!isClosed()) {
        stopAndJoinWorker();
        onClose(mWorker->setClosed());
        return ndk::ScopedAStatus::ok();
    } else {
        LOG(ERROR) << __func__ << ": stream was already closed";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
}

ndk::ScopedAStatus StreamCommonImpl::prepareToClose() {
    LOG(DEBUG) << __func__;
    if (!isClosed()) {
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": stream was closed";
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

void StreamCommonImpl::cleanupWorker() {
    if (!isClosed()) {
        LOG(ERROR) << __func__ << ": stream was not closed prior to destruction, resource leak";
        stopAndJoinWorker();
    }
}

void StreamCommonImpl::setWorkerThreadPriority(pid_t workerTid) {
    // FAST workers should be run with a SCHED_FIFO scheduler, however the host process
    // might be lacking the capability to request it, thus a failure to set is not an error.
    if (auto flags = getContext().getFlags();
        (flags.getTag() == AudioIoFlags::Tag::input &&
         isBitPositionFlagSet(flags.template get<AudioIoFlags::Tag::input>(),
                              AudioInputFlags::FAST)) ||
        (flags.getTag() == AudioIoFlags::Tag::output &&
         (isBitPositionFlagSet(flags.template get<AudioIoFlags::Tag::output>(),
                               AudioOutputFlags::FAST) ||
          isBitPositionFlagSet(flags.template get<AudioIoFlags::Tag::output>(),
                               AudioOutputFlags::SPATIALIZER)))) {
        constexpr int32_t kRTPriorityMin = 1;  // SchedulingPolicyService.PRIORITY_MIN (Java).
        constexpr int32_t kRTPriorityMax = 3;  // SchedulingPolicyService.PRIORITY_MAX (Java).
        int priorityBoost = kRTPriorityMax;
        if (flags.getTag() == AudioIoFlags::Tag::output &&
            isBitPositionFlagSet(flags.template get<AudioIoFlags::Tag::output>(),
                                 AudioOutputFlags::SPATIALIZER)) {
            const int32_t sptPrio =
                    property_get_int32("audio.spatializer.priority", kRTPriorityMin);
            if (sptPrio >= kRTPriorityMin && sptPrio <= kRTPriorityMax) {
                priorityBoost = sptPrio;
            } else {
                LOG(WARNING) << __func__ << ": invalid spatializer priority: " << sptPrio;
                return;
            }
        }
        struct sched_param param = {
                .sched_priority = priorityBoost,
        };
        if (sched_setscheduler(workerTid, SCHED_FIFO | SCHED_RESET_ON_FORK, &param) != 0) {
            PLOG(WARNING) << __func__ << ": failed to set FIFO scheduler and priority";
        }
    }
}

void StreamCommonImpl::stopAndJoinWorker() {
    stopWorker();
    LOG(DEBUG) << __func__ << ": joining the worker thread...";
    mWorker->join();
    LOG(DEBUG) << __func__ << ": worker thread joined";
}

void StreamCommonImpl::stopWorker() {
    if (auto commandMQ = mContext.getCommandMQ(); commandMQ != nullptr) {
        LOG(DEBUG) << __func__ << ": asking the worker to exit...";
        auto cmd = StreamDescriptor::Command::make<StreamDescriptor::Command::Tag::halReservedExit>(
                mContext.getInternalCommandCookie() ^ mWorker->getTid());
        // Note: never call 'pause' and 'resume' methods of StreamWorker
        // in the HAL implementation. These methods are to be used by
        // the client side only. Preventing the worker loop from running
        // on the HAL side can cause a deadlock.
        if (!commandMQ->writeBlocking(&cmd, 1)) {
            LOG(ERROR) << __func__ << ": failed to write exit command to the MQ";
        }
        LOG(DEBUG) << __func__ << ": done";
    }
    mWorkerStopIssued = true;
}

ndk::ScopedAStatus StreamCommonImpl::updateMetadataCommon(const Metadata& metadata) {
    LOG(DEBUG) << __func__;
    if (!isClosed()) {
        if (metadata.index() != mMetadata.index()) {
            LOG(FATAL) << __func__ << ": changing metadata variant is not allowed";
        }
        mMetadata = metadata;
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": stream was closed";
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

ndk::ScopedAStatus StreamCommonImpl::setConnectedDevices(
        const std::vector<::aidl::android::media::audio::common::AudioDevice>& devices) {
    mWorker->setIsConnected(!devices.empty());
    mConnectedDevices = devices;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamCommonImpl::setGain(float gain) {
    LOG(DEBUG) << __func__ << ": gain " << gain;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamCommonImpl::bluetoothParametersUpdated() {
    LOG(DEBUG) << __func__;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

namespace {
static std::map<AudioDevice, std::string> transformMicrophones(
        const std::vector<MicrophoneInfo>& microphones) {
    std::map<AudioDevice, std::string> result;
    std::transform(microphones.begin(), microphones.end(), std::inserter(result, result.begin()),
                   [](const auto& mic) { return std::make_pair(mic.device, mic.id); });
    return result;
}
}  // namespace

StreamIn::StreamIn(StreamContext&& context, const std::vector<MicrophoneInfo>& microphones)
    : mContextInstance(std::move(context)), mMicrophones(transformMicrophones(microphones)) {
    LOG(DEBUG) << __func__;
}

void StreamIn::defaultOnClose() {
    mContextInstance.reset();
}

ndk::ScopedAStatus StreamIn::getActiveMicrophones(
        std::vector<MicrophoneDynamicInfo>* _aidl_return) {
    std::vector<MicrophoneDynamicInfo> result;
    std::vector<MicrophoneDynamicInfo::ChannelMapping> channelMapping{
            getChannelCount(getContext().getChannelLayout()),
            MicrophoneDynamicInfo::ChannelMapping::DIRECT};
    for (auto it = getConnectedDevices().begin(); it != getConnectedDevices().end(); ++it) {
        if (auto micIt = mMicrophones.find(*it); micIt != mMicrophones.end()) {
            MicrophoneDynamicInfo dynMic;
            dynMic.id = micIt->second;
            dynMic.channelMapping = channelMapping;
            result.push_back(std::move(dynMic));
        }
    }
    *_aidl_return = std::move(result);
    LOG(DEBUG) << __func__ << ": returning " << ::android::internal::ToString(*_aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamIn::getMicrophoneDirection(MicrophoneDirection* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamIn::setMicrophoneDirection(MicrophoneDirection in_direction) {
    LOG(DEBUG) << __func__ << ": direction " << toString(in_direction);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamIn::getMicrophoneFieldDimension(float* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamIn::setMicrophoneFieldDimension(float in_zoom) {
    LOG(DEBUG) << __func__ << ": zoom " << in_zoom;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamIn::getHwGain(std::vector<float>* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamIn::setHwGain(const std::vector<float>& in_channelGains) {
    LOG(DEBUG) << __func__ << ": gains " << ::android::internal::ToString(in_channelGains);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

StreamInHwGainHelper::StreamInHwGainHelper(const StreamContext* context)
    : mChannelCount(getChannelCount(context->getChannelLayout())) {}

ndk::ScopedAStatus StreamInHwGainHelper::getHwGainImpl(std::vector<float>* _aidl_return) {
    if (mHwGains.empty()) {
        mHwGains.resize(mChannelCount, 0.0f);
    }
    *_aidl_return = mHwGains;
    LOG(DEBUG) << __func__ << ": returning " << ::android::internal::ToString(*_aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamInHwGainHelper::setHwGainImpl(const std::vector<float>& in_channelGains) {
    LOG(DEBUG) << __func__ << ": gains " << ::android::internal::ToString(in_channelGains);
    if (in_channelGains.size() != mChannelCount) {
        LOG(ERROR) << __func__
                   << ": channel count does not match stream channel count: " << mChannelCount;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    for (float gain : in_channelGains) {
        if (gain < StreamIn::HW_GAIN_MIN || gain > StreamIn::HW_GAIN_MAX) {
            LOG(ERROR) << __func__ << ": gain value out of range: " << gain;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
    }
    mHwGains = in_channelGains;
    return ndk::ScopedAStatus::ok();
}

StreamOut::StreamOut(StreamContext&& context, const std::optional<AudioOffloadInfo>& offloadInfo)
    : mContextInstance(std::move(context)), mOffloadInfo(offloadInfo) {
    LOG(DEBUG) << __func__;
}

void StreamOut::defaultOnClose() {
    mContextInstance.reset();
}

ndk::ScopedAStatus StreamOut::updateOffloadMetadata(
        const AudioOffloadMetadata& in_offloadMetadata) {
    LOG(DEBUG) << __func__;
    if (isClosed()) {
        LOG(ERROR) << __func__ << ": stream was closed";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (!mOffloadInfo.has_value()) {
        LOG(ERROR) << __func__ << ": not a compressed offload stream";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (in_offloadMetadata.sampleRate < 0) {
        LOG(ERROR) << __func__ << ": invalid sample rate value: " << in_offloadMetadata.sampleRate;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_offloadMetadata.averageBitRatePerSecond < 0) {
        LOG(ERROR) << __func__
                   << ": invalid average BPS value: " << in_offloadMetadata.averageBitRatePerSecond;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_offloadMetadata.delayFrames < 0) {
        LOG(ERROR) << __func__
                   << ": invalid delay frames value: " << in_offloadMetadata.delayFrames;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_offloadMetadata.paddingFrames < 0) {
        LOG(ERROR) << __func__
                   << ": invalid padding frames value: " << in_offloadMetadata.paddingFrames;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    mOffloadMetadata = in_offloadMetadata;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOut::getHwVolume(std::vector<float>* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::setHwVolume(const std::vector<float>& in_channelVolumes) {
    LOG(DEBUG) << __func__ << ": gains " << ::android::internal::ToString(in_channelVolumes);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::getAudioDescriptionMixLevel(float* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::setAudioDescriptionMixLevel(float in_leveldB) {
    LOG(DEBUG) << __func__ << ": description mix level " << in_leveldB;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::getDualMonoMode(AudioDualMonoMode* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::setDualMonoMode(AudioDualMonoMode in_mode) {
    LOG(DEBUG) << __func__ << ": dual mono mode " << toString(in_mode);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::getRecommendedLatencyModes(
        std::vector<AudioLatencyMode>* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::setLatencyMode(AudioLatencyMode in_mode) {
    LOG(DEBUG) << __func__ << ": latency mode " << toString(in_mode);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::getPlaybackRateParameters(AudioPlaybackRate* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::setPlaybackRateParameters(const AudioPlaybackRate& in_playbackRate) {
    LOG(DEBUG) << __func__ << ": " << in_playbackRate.toString();
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOut::selectPresentation(int32_t in_presentationId, int32_t in_programId) {
    LOG(DEBUG) << __func__ << ": presentationId " << in_presentationId << ", programId "
               << in_programId;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

StreamOutHwVolumeHelper::StreamOutHwVolumeHelper(const StreamContext* context)
    : mChannelCount(getChannelCount(context->getChannelLayout())) {}

ndk::ScopedAStatus StreamOutHwVolumeHelper::getHwVolumeImpl(std::vector<float>* _aidl_return) {
    if (mHwVolumes.empty()) {
        mHwVolumes.resize(mChannelCount, 0.0f);
    }
    *_aidl_return = mHwVolumes;
    LOG(DEBUG) << __func__ << ": returning " << ::android::internal::ToString(*_aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOutHwVolumeHelper::setHwVolumeImpl(
        const std::vector<float>& in_channelVolumes) {
    LOG(DEBUG) << __func__ << ": volumes " << ::android::internal::ToString(in_channelVolumes);
    if (in_channelVolumes.size() != mChannelCount) {
        LOG(ERROR) << __func__
                   << ": channel count does not match stream channel count: " << mChannelCount;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    for (float volume : in_channelVolumes) {
        if (volume < StreamOut::HW_VOLUME_MIN || volume > StreamOut::HW_VOLUME_MAX) {
            LOG(ERROR) << __func__ << ": volume value out of range: " << volume;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
    }
    mHwVolumes = in_channelVolumes;
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core
