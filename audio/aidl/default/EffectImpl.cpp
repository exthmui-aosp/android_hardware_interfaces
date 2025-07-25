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

#include <memory>
#define ATRACE_TAG ATRACE_TAG_AUDIO
#define LOG_TAG "AHAL_EffectImpl"
#include <utils/Trace.h>
#include "effect-impl/EffectImpl.h"
#include "effect-impl/EffectTypes.h"
#include "include/effect-impl/EffectTypes.h"

using aidl::android::hardware::audio::effect::CommandId;
using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::kDestroyAnyStateSupportedVersion;
using aidl::android::hardware::audio::effect::kEventFlagDataMqNotEmpty;
using aidl::android::hardware::audio::effect::kEventFlagNotEmpty;
using aidl::android::hardware::audio::effect::kReopenSupportedVersion;
using aidl::android::hardware::audio::effect::State;
using aidl::android::media::audio::common::PcmType;
using ::android::hardware::EventFlag;

extern "C" binder_exception_t destroyEffect(const std::shared_ptr<IEffect>& instanceSp) {
    if (!instanceSp) {
        LOG(ERROR) << __func__ << " nullptr";
        return EX_ILLEGAL_ARGUMENT;
    }

    Descriptor desc;
    ndk::ScopedAStatus status = instanceSp->getDescriptor(&desc);
    if (!status.isOk()) {
        LOG(ERROR) << __func__ << " instance " << instanceSp.get()
                   << " failed to get descriptor, status: " << status.getDescription();
        return EX_ILLEGAL_STATE;
    }

    State state;
    status = instanceSp->getState(&state);
    if (!status.isOk()) {
        LOG(ERROR) << __func__ << " " << desc.common.name << " instance " << instanceSp.get()
                   << " in state: " << toString(state) << ", status: " << status.getDescription();
        return EX_ILLEGAL_STATE;
    }

    int effectVersion = 0;
    if (!instanceSp->getInterfaceVersion(&effectVersion).isOk()) {
        LOG(WARNING) << __func__ << " " << desc.common.name << " failed to get interface version";
    }

    if (effectVersion < kDestroyAnyStateSupportedVersion) {
        if (State::INIT != state) {
            LOG(ERROR) << __func__ << " " << desc.common.name << " can not destroy instance "
                       << instanceSp.get() << " in state: " << toString(state);
            return EX_ILLEGAL_STATE;
        }
    } else {
        instanceSp->command(CommandId::RESET);
        instanceSp->close();
    }

    LOG(DEBUG) << __func__ << " " << desc.common.name << " instance " << instanceSp.get()
               << " destroyed";
    return EX_NONE;
}

namespace aidl::android::hardware::audio::effect {

ndk::ScopedAStatus EffectImpl::open(const Parameter::Common& common,
                                    const std::optional<Parameter::Specific>& specific,
                                    OpenEffectReturn* ret) {
    // effect only support 32bits float
    RETURN_IF(common.input.base.format.pcm != common.output.base.format.pcm ||
                      common.input.base.format.pcm != PcmType::FLOAT_32_BIT,
              EX_ILLEGAL_ARGUMENT, "dataMustBe32BitsFloat");

    std::lock_guard lg(mImplMutex);
    RETURN_OK_IF(mState != State::INIT);
    mImplContext = createContext(common);
    RETURN_IF(!mImplContext, EX_NULL_POINTER, "nullContext");

    RETURN_IF(!getInterfaceVersion(&mVersion).isOk(), EX_UNSUPPORTED_OPERATION,
              "FailedToGetInterfaceVersion");
    mImplContext->setVersion(mVersion);
    mEventFlag = mImplContext->getStatusEventFlag();
    mDataMqNotEmptyEf =
            mVersion >= kReopenSupportedVersion ? kEventFlagDataMqNotEmpty : kEventFlagNotEmpty;

    if (specific.has_value()) {
        RETURN_IF_ASTATUS_NOT_OK(setParameterSpecific(specific.value()), "setSpecParamErr");
    }

    mState = State::IDLE;
    mImplContext->dupeFmq(ret);
    RETURN_IF(createThread(getEffectNameWithVersion()) != RetCode::SUCCESS,
              EX_UNSUPPORTED_OPERATION, "FailedToCreateWorker");
    LOG(INFO) << getEffectNameWithVersion() << __func__;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus EffectImpl::reopen(OpenEffectReturn* ret) {
    std::lock_guard lg(mImplMutex);
    RETURN_IF(mState == State::INIT, EX_ILLEGAL_STATE, "alreadyClosed");

    RETURN_IF(!mImplContext, EX_NULL_POINTER, "nullContext");
    mImplContext->dupeFmqWithReopen(ret);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus EffectImpl::close() {
    {
        std::lock_guard lg(mImplMutex);
        RETURN_OK_IF(mState == State::INIT);
        RETURN_IF(mState == State::PROCESSING, EX_ILLEGAL_STATE, "closeAtProcessing");
        mState = State::INIT;
    }

    RETURN_IF(notifyEventFlag(mDataMqNotEmptyEf) != RetCode::SUCCESS, EX_ILLEGAL_STATE,
              "notifyEventFlagNotEmptyFailed");
    // stop the worker thread, ignore the return code
    RETURN_IF(destroyThread() != RetCode::SUCCESS, EX_UNSUPPORTED_OPERATION,
              "FailedToDestroyWorker");

    {
        std::lock_guard lg(mImplMutex);
        releaseContext();
        mImplContext.reset();
    }

    LOG(INFO) << getEffectNameWithVersion() << __func__;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus EffectImpl::setParameter(const Parameter& param) {
    std::lock_guard lg(mImplMutex);
    LOG(VERBOSE) << getEffectNameWithVersion() << __func__ << " with: " << param.toString();

    const auto& tag = param.getTag();
    switch (tag) {
        case Parameter::common:
        case Parameter::deviceDescription:
        case Parameter::mode:
        case Parameter::source:
            FALLTHROUGH_INTENDED;
        case Parameter::volumeStereo:
            return setParameterCommon(param);
        case Parameter::specific: {
            return setParameterSpecific(param.get<Parameter::specific>());
        }
        default: {
            LOG(ERROR) << getEffectNameWithVersion() << __func__ << " unsupportedParameterTag "
                       << toString(tag);
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "ParameterNotSupported");
        }
    }
}

ndk::ScopedAStatus EffectImpl::getParameter(const Parameter::Id& id, Parameter* param) {
    std::lock_guard lg(mImplMutex);
    switch (id.getTag()) {
        case Parameter::Id::commonTag: {
            RETURN_IF_ASTATUS_NOT_OK(getParameterCommon(id.get<Parameter::Id::commonTag>(), param),
                                     "CommonParamNotSupported");
            break;
        }
        case Parameter::Id::vendorEffectTag:
            FALLTHROUGH_INTENDED;
        default: {
            Parameter::Specific specific;
            RETURN_IF_ASTATUS_NOT_OK(getParameterSpecific(id, &specific), "SpecParamNotSupported");
            param->set<Parameter::specific>(specific);
            break;
        }
    }
    LOG(VERBOSE) << getEffectNameWithVersion() << __func__ << id.toString() << param->toString();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus EffectImpl::setParameterCommon(const Parameter& param) {
    RETURN_IF(!mImplContext, EX_NULL_POINTER, "nullContext");

    const auto& tag = param.getTag();
    switch (tag) {
        case Parameter::common:
            RETURN_IF(mImplContext->setCommon(param.get<Parameter::common>()) != RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setCommFailed");
            break;
        case Parameter::deviceDescription:
            RETURN_IF(mImplContext->setOutputDevice(param.get<Parameter::deviceDescription>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setDeviceFailed");
            break;
        case Parameter::mode:
            RETURN_IF(mImplContext->setAudioMode(param.get<Parameter::mode>()) != RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setModeFailed");
            break;
        case Parameter::source:
            RETURN_IF(mImplContext->setAudioSource(param.get<Parameter::source>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setSourceFailed");
            break;
        case Parameter::volumeStereo:
            RETURN_IF(mImplContext->setVolumeStereo(param.get<Parameter::volumeStereo>()) !=
                              RetCode::SUCCESS,
                      EX_ILLEGAL_ARGUMENT, "setVolumeStereoFailed");
            break;
        default: {
            LOG(ERROR) << getEffectNameWithVersion() << __func__ << " unsupportedParameterTag "
                       << toString(tag);
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "commonParamNotSupported");
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus EffectImpl::getParameterCommon(const Parameter::Tag& tag, Parameter* param) {
    RETURN_IF(!mImplContext, EX_NULL_POINTER, "nullContext");

    switch (tag) {
        case Parameter::common: {
            param->set<Parameter::common>(mImplContext->getCommon());
            break;
        }
        case Parameter::deviceDescription: {
            param->set<Parameter::deviceDescription>(mImplContext->getOutputDevice());
            break;
        }
        case Parameter::mode: {
            param->set<Parameter::mode>(mImplContext->getAudioMode());
            break;
        }
        case Parameter::source: {
            param->set<Parameter::source>(mImplContext->getAudioSource());
            break;
        }
        case Parameter::volumeStereo: {
            param->set<Parameter::volumeStereo>(mImplContext->getVolumeStereo());
            break;
        }
        default: {
            LOG(DEBUG) << getEffectNameWithVersion() << __func__ << " unsupported tag "
                       << toString(tag);
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "tagNotSupported");
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus EffectImpl::getState(State* state) NO_THREAD_SAFETY_ANALYSIS {
    *state = mState;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus EffectImpl::command(CommandId command) {
    std::lock_guard lg(mImplMutex);
    RETURN_IF(mState == State::INIT, EX_ILLEGAL_STATE, "instanceNotOpen");

    switch (command) {
        case CommandId::START:
            RETURN_OK_IF(mState == State::PROCESSING);
            RETURN_IF_ASTATUS_NOT_OK(commandImpl(command), "commandImplFailed");
            mState = State::PROCESSING;
            RETURN_IF(notifyEventFlag(mDataMqNotEmptyEf) != RetCode::SUCCESS, EX_ILLEGAL_STATE,
                      "notifyEventFlagNotEmptyFailed");
            startThread();
            break;
        case CommandId::STOP:
            RETURN_OK_IF(mState == State::IDLE);
            mState = State::IDLE;
            RETURN_IF(notifyEventFlag(mDataMqNotEmptyEf) != RetCode::SUCCESS, EX_ILLEGAL_STATE,
                      "notifyEventFlagNotEmptyFailed");
            stopThread();
            RETURN_IF_ASTATUS_NOT_OK(commandImpl(command), "commandImplFailed");
            break;
        case CommandId::RESET:
            mState = State::IDLE;
            RETURN_IF(notifyEventFlag(mDataMqNotEmptyEf) != RetCode::SUCCESS, EX_ILLEGAL_STATE,
                      "notifyEventFlagNotEmptyFailed");
            stopThread();
            RETURN_IF_ASTATUS_NOT_OK(commandImpl(command), "commandImplFailed");
            break;
        default:
            LOG(ERROR) << getEffectNameWithVersion() << __func__ << " instance still processing";
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "CommandIdNotSupported");
    }
    LOG(VERBOSE) << getEffectNameWithVersion() << __func__
                 << " transfer to state: " << toString(mState);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus EffectImpl::commandImpl(CommandId command) {
    RETURN_IF(!mImplContext, EX_NULL_POINTER, "nullContext");
    switch (command) {
        case CommandId::START:
            mImplContext->enable();
            break;
        case CommandId::STOP:
            mImplContext->disable();
            break;
        case CommandId::RESET:
            mImplContext->disable();
            mImplContext->reset();
            mImplContext->resetBuffer();
            break;
        default:
            LOG(ERROR) << __func__ << " commandId " << toString(command) << " not supported";
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "commandIdNotSupported");
    }
    return ndk::ScopedAStatus::ok();
}

std::shared_ptr<EffectContext> EffectImpl::createContext(const Parameter::Common& common) {
    return std::make_shared<EffectContext>(1 /* statusMqDepth */, common);
}

RetCode EffectImpl::releaseContext() {
    if (mImplContext) {
        mImplContext.reset();
    }
    return RetCode::SUCCESS;
}

void EffectImpl::cleanUp() {
    command(CommandId::STOP);
    close();
}

RetCode EffectImpl::notifyEventFlag(uint32_t flag) {
    if (!mEventFlag) {
        LOG(ERROR) << getEffectNameWithVersion() << __func__ << ": StatusEventFlag invalid";
        return RetCode::ERROR_EVENT_FLAG_ERROR;
    }
    if (const auto ret = mEventFlag->wake(flag); ret != ::android::OK) {
        LOG(ERROR) << getEffectNameWithVersion() << __func__ << ": wake failure with ret " << ret;
        return RetCode::ERROR_EVENT_FLAG_ERROR;
    }
    LOG(VERBOSE) << getEffectNameWithVersion() << __func__ << ": " << std::hex << mEventFlag;
    return RetCode::SUCCESS;
}

IEffect::Status EffectImpl::status(binder_status_t status, size_t consumed, size_t produced) {
    IEffect::Status ret;
    ret.status = status;
    ret.fmqConsumed = consumed;
    ret.fmqProduced = produced;
    return ret;
}

void EffectImpl::process() {
    ATRACE_NAME(getEffectNameWithVersion().c_str());
    /**
     * wait for the EventFlag without lock, it's ok because the mEfGroup pointer will not change
     * in the life cycle of workerThread (threadLoop).
     */
    uint32_t efState = 0;
    if (!mEventFlag ||
        ::android::OK != mEventFlag->wait(mDataMqNotEmptyEf, &efState, 0 /* no timeout */,
                                          true /* retry */) ||
        !(efState & mDataMqNotEmptyEf)) {
        LOG(ERROR) << getEffectNameWithVersion() << __func__ << ": StatusEventFlag - " << mEventFlag
                   << " efState - " << std::hex << efState;
        return;
    }

    {
        std::lock_guard lg(mImplMutex);
        if (mState != State::PROCESSING && mState != State::DRAINING) {
            LOG(DEBUG) << getEffectNameWithVersion()
                       << " skip process in state: " << toString(mState);
            return;
        }
        RETURN_VALUE_IF(!mImplContext, void(), "nullContext");
        auto statusMQ = mImplContext->getStatusFmq();
        auto inputMQ = mImplContext->getInputDataFmq();
        auto outputMQ = mImplContext->getOutputDataFmq();
        auto buffer = mImplContext->getWorkBuffer();
        if (!inputMQ || !outputMQ) {
            return;
        }

        assert(mImplContext->getWorkBufferSize() >=
               std::max(inputMQ->availableToRead(), outputMQ->availableToWrite()));
        auto processSamples = std::min(inputMQ->availableToRead(), outputMQ->availableToWrite());
        if (processSamples) {
            inputMQ->read(buffer, processSamples);
            IEffect::Status status = effectProcessImpl(buffer, buffer, processSamples);
            outputMQ->write(buffer, status.fmqProduced);
            statusMQ->writeBlocking(&status, 1);
        }
    }
}

// A placeholder processing implementation to copy samples from input to output
IEffect::Status EffectImpl::effectProcessImpl(float* in, float* out, int samples) {
    for (int i = 0; i < samples; i++) {
        *out++ = *in++;
    }
    return {STATUS_OK, samples, samples};
}

}  // namespace aidl::android::hardware::audio::effect
