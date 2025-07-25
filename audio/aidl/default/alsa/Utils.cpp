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

#include <map>
#include <set>

#define LOG_TAG "AHAL_AlsaUtils"
#include <Utils.h>
#include <aidl/android/media/audio/common/AudioFormatType.h>
#include <aidl/android/media/audio/common/PcmType.h>
#include <android-base/logging.h>
#include <audio_utils/primitives.h>
#include <cutils/compiler.h>

#include "Utils.h"
#include "core-impl/utils.h"

using aidl::android::hardware::audio::common::getChannelCount;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioFormatDescription;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::PcmType;

namespace aidl::android::hardware::audio::core::alsa {

const float kUnityGainFloat = 1.0f;

DeviceProxy::DeviceProxy() : mProfile(nullptr), mProxy(nullptr, alsaProxyDeleter) {}

DeviceProxy::DeviceProxy(const DeviceProfile& deviceProfile)
    : mProfile(new alsa_device_profile), mProxy(new alsa_device_proxy, alsaProxyDeleter) {
    profile_init(mProfile.get(), deviceProfile.direction);
    mProfile->card = deviceProfile.card;
    mProfile->device = deviceProfile.device;
    memset(mProxy.get(), 0, sizeof(alsa_device_proxy));
}

void DeviceProxy::alsaProxyDeleter(alsa_device_proxy* proxy) {
    if (proxy != nullptr) {
        proxy_close(proxy);
        delete proxy;
    }
}

namespace {

using AudioChannelCountToMaskMap = std::map<unsigned int, AudioChannelLayout>;
using AudioFormatDescToPcmFormatMap = std::map<AudioFormatDescription, enum pcm_format>;
using PcmFormatToAudioFormatDescMap = std::map<enum pcm_format, AudioFormatDescription>;

AudioChannelLayout getInvalidChannelLayout() {
    static const AudioChannelLayout invalidChannelLayout =
            AudioChannelLayout::make<AudioChannelLayout::Tag::invalid>(0);
    return invalidChannelLayout;
}

static AudioChannelCountToMaskMap make_ChannelCountToMaskMap(
        const std::set<AudioChannelLayout>& channelMasks) {
    AudioChannelCountToMaskMap channelMaskToCountMap;
    for (const auto& channelMask : channelMasks) {
        channelMaskToCountMap.emplace(getChannelCount(channelMask), channelMask);
    }
    return channelMaskToCountMap;
}

#define DEFINE_CHANNEL_LAYOUT_MASK(n) \
    AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(AudioChannelLayout::LAYOUT_##n)

const AudioChannelCountToMaskMap& getSupportedChannelOutLayoutMap() {
    static const std::set<AudioChannelLayout> supportedOutChannelLayouts = {
            DEFINE_CHANNEL_LAYOUT_MASK(MONO),
            DEFINE_CHANNEL_LAYOUT_MASK(STEREO),
    };
    static const AudioChannelCountToMaskMap outLayouts =
            make_ChannelCountToMaskMap(supportedOutChannelLayouts);
    return outLayouts;
}

const AudioChannelCountToMaskMap& getSupportedChannelInLayoutMap() {
    static const std::set<AudioChannelLayout> supportedInChannelLayouts = {
            DEFINE_CHANNEL_LAYOUT_MASK(MONO),
            DEFINE_CHANNEL_LAYOUT_MASK(STEREO),
    };
    static const AudioChannelCountToMaskMap inLayouts =
            make_ChannelCountToMaskMap(supportedInChannelLayouts);
    return inLayouts;
}

#undef DEFINE_CHANNEL_LAYOUT_MASK
#define DEFINE_CHANNEL_INDEX_MASK(n) \
    AudioChannelLayout::make<AudioChannelLayout::Tag::indexMask>(AudioChannelLayout::INDEX_MASK_##n)

const AudioChannelCountToMaskMap& getSupportedChannelIndexLayoutMap() {
    static const std::set<AudioChannelLayout> supportedIndexChannelLayouts = {
            DEFINE_CHANNEL_INDEX_MASK(1),  DEFINE_CHANNEL_INDEX_MASK(2),
            DEFINE_CHANNEL_INDEX_MASK(3),  DEFINE_CHANNEL_INDEX_MASK(4),
            DEFINE_CHANNEL_INDEX_MASK(5),  DEFINE_CHANNEL_INDEX_MASK(6),
            DEFINE_CHANNEL_INDEX_MASK(7),  DEFINE_CHANNEL_INDEX_MASK(8),
            DEFINE_CHANNEL_INDEX_MASK(9),  DEFINE_CHANNEL_INDEX_MASK(10),
            DEFINE_CHANNEL_INDEX_MASK(11), DEFINE_CHANNEL_INDEX_MASK(12),
            DEFINE_CHANNEL_INDEX_MASK(13), DEFINE_CHANNEL_INDEX_MASK(14),
            DEFINE_CHANNEL_INDEX_MASK(15), DEFINE_CHANNEL_INDEX_MASK(16),
            DEFINE_CHANNEL_INDEX_MASK(17), DEFINE_CHANNEL_INDEX_MASK(18),
            DEFINE_CHANNEL_INDEX_MASK(19), DEFINE_CHANNEL_INDEX_MASK(20),
            DEFINE_CHANNEL_INDEX_MASK(21), DEFINE_CHANNEL_INDEX_MASK(22),
            DEFINE_CHANNEL_INDEX_MASK(23), DEFINE_CHANNEL_INDEX_MASK(24),
    };
    static const AudioChannelCountToMaskMap indexLayouts =
            make_ChannelCountToMaskMap(supportedIndexChannelLayouts);
    return indexLayouts;
}

#undef DEFINE_CHANNEL_INDEX_MASK

AudioFormatDescription make_AudioFormatDescription(AudioFormatType type) {
    AudioFormatDescription result;
    result.type = type;
    return result;
}

AudioFormatDescription make_AudioFormatDescription(PcmType pcm) {
    auto result = make_AudioFormatDescription(AudioFormatType::PCM);
    result.pcm = pcm;
    return result;
}

const AudioFormatDescToPcmFormatMap& getAudioFormatDescriptorToPcmFormatMap() {
    static const AudioFormatDescToPcmFormatMap formatDescToPcmFormatMap = {
            {make_AudioFormatDescription(PcmType::INT_16_BIT), PCM_FORMAT_S16_LE},
            {make_AudioFormatDescription(PcmType::FIXED_Q_8_24), PCM_FORMAT_S24_LE},
            {make_AudioFormatDescription(PcmType::INT_24_BIT), PCM_FORMAT_S24_3LE},
            {make_AudioFormatDescription(PcmType::INT_32_BIT), PCM_FORMAT_S32_LE},
            {make_AudioFormatDescription(PcmType::FLOAT_32_BIT), PCM_FORMAT_FLOAT_LE},
    };
    return formatDescToPcmFormatMap;
}

static PcmFormatToAudioFormatDescMap make_PcmFormatToAudioFormatDescMap(
        const AudioFormatDescToPcmFormatMap& formatDescToPcmFormatMap) {
    PcmFormatToAudioFormatDescMap result;
    for (const auto& formatPair : formatDescToPcmFormatMap) {
        result.emplace(formatPair.second, formatPair.first);
    }
    return result;
}

const PcmFormatToAudioFormatDescMap& getPcmFormatToAudioFormatDescMap() {
    static const PcmFormatToAudioFormatDescMap pcmFormatToFormatDescMap =
            make_PcmFormatToAudioFormatDescMap(getAudioFormatDescriptorToPcmFormatMap());
    return pcmFormatToFormatDescMap;
}

void applyGainToInt16Buffer(void* buffer, const size_t bufferSizeBytes, const float gain,
                            int channelCount) {
    const uint16_t unityGainQ4_12 = u4_12_from_float(kUnityGainFloat);
    const uint16_t vl = u4_12_from_float(gain);
    const uint32_t vrl = (vl << 16) | vl;
    int numFrames = 0;
    if (channelCount == 2) {
        numFrames = bufferSizeBytes / sizeof(uint32_t);
        if (numFrames == 0) {
            return;
        }
        uint32_t* intBuffer = (uint32_t*)buffer;
        if (CC_UNLIKELY(vl > unityGainQ4_12)) {
            do {
                int32_t l = mulRL(1, *intBuffer, vrl) >> 12;
                int32_t r = mulRL(0, *intBuffer, vrl) >> 12;
                l = clamp16(l);
                r = clamp16(r);
                *intBuffer++ = (r << 16) | (l & 0xFFFF);
            } while (--numFrames);
        } else {
            do {
                int32_t l = mulRL(1, *intBuffer, vrl) >> 12;
                int32_t r = mulRL(0, *intBuffer, vrl) >> 12;
                *intBuffer++ = (r << 16) | (l & 0xFFFF);
            } while (--numFrames);
        }
    } else {
        numFrames = bufferSizeBytes / sizeof(uint16_t);
        if (numFrames == 0) {
            return;
        }
        int16_t* intBuffer = (int16_t*)buffer;
        if (CC_UNLIKELY(vl > unityGainQ4_12)) {
            do {
                int32_t mono = mul(*intBuffer, static_cast<int16_t>(vl)) >> 12;
                *intBuffer++ = clamp16(mono);
            } while (--numFrames);
        } else {
            do {
                int32_t mono = mul(*intBuffer, static_cast<int16_t>(vl)) >> 12;
                *intBuffer++ = static_cast<int16_t>(mono & 0xFFFF);
            } while (--numFrames);
        }
    }
}

void applyGainToInt32Buffer(int32_t* typedBuffer, const size_t bufferSizeBytes, const float gain) {
    int numSamples = bufferSizeBytes / sizeof(int32_t);
    if (numSamples == 0) {
        return;
    }
    if (CC_UNLIKELY(gain > kUnityGainFloat)) {
        do {
            float multiplied = (*typedBuffer) * gain;
            if (multiplied > INT32_MAX) {
                *typedBuffer++ = INT32_MAX;
            } else if (multiplied < INT32_MIN) {
                *typedBuffer++ = INT32_MIN;
            } else {
                *typedBuffer++ = multiplied;
            }
        } while (--numSamples);
    } else {
        do {
            *typedBuffer++ = (*typedBuffer) * gain;
        } while (--numSamples);
    }
}

void applyGainToFloatBuffer(float* floatBuffer, const size_t bufferSizeBytes, const float gain) {
    int numSamples = bufferSizeBytes / sizeof(float);
    if (numSamples == 0) {
        return;
    }
    if (CC_UNLIKELY(gain > kUnityGainFloat)) {
        do {
            *floatBuffer++ = std::clamp((*floatBuffer) * gain, -kUnityGainFloat, kUnityGainFloat);
        } while (--numSamples);
    } else {
        do {
            *floatBuffer++ = (*floatBuffer) * gain;
        } while (--numSamples);
    }
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const DeviceProfile& device) {
    return os << "<" << device.card << "," << device.device << ">";
}

AudioChannelLayout getChannelLayoutMaskFromChannelCount(unsigned int channelCount, int isInput) {
    return findValueOrDefault(
            isInput ? getSupportedChannelInLayoutMap() : getSupportedChannelOutLayoutMap(),
            channelCount, getInvalidChannelLayout());
}

AudioChannelLayout getChannelIndexMaskFromChannelCount(unsigned int channelCount) {
    return findValueOrDefault(getSupportedChannelIndexLayoutMap(), channelCount,
                              getInvalidChannelLayout());
}

unsigned int getChannelCountFromChannelMask(const AudioChannelLayout& channelMask, bool isInput) {
    switch (channelMask.getTag()) {
        case AudioChannelLayout::Tag::layoutMask: {
            return findKeyOrDefault(
                    isInput ? getSupportedChannelInLayoutMap() : getSupportedChannelOutLayoutMap(),
                    static_cast<unsigned>(getChannelCount(channelMask)), 0u /*defaultValue*/);
        }
        case AudioChannelLayout::Tag::indexMask: {
            return findKeyOrDefault(getSupportedChannelIndexLayoutMap(),
                                    static_cast<unsigned>(getChannelCount(channelMask)),
                                    0u /*defaultValue*/);
        }
        case AudioChannelLayout::Tag::none:
        case AudioChannelLayout::Tag::invalid:
        case AudioChannelLayout::Tag::voiceMask:
        default:
            return 0;
    }
}

std::vector<AudioChannelLayout> getChannelMasksFromProfile(const alsa_device_profile* profile) {
    const bool isInput = profile->direction == PCM_IN;
    std::vector<AudioChannelLayout> channels;
    for (size_t i = 0; i < AUDIO_PORT_MAX_CHANNEL_MASKS && profile->channel_counts[i] != 0; ++i) {
        auto layoutMask =
                alsa::getChannelLayoutMaskFromChannelCount(profile->channel_counts[i], isInput);
        if (layoutMask.getTag() == AudioChannelLayout::Tag::layoutMask) {
            channels.push_back(layoutMask);
        }
        auto indexMask = alsa::getChannelIndexMaskFromChannelCount(profile->channel_counts[i]);
        if (indexMask.getTag() == AudioChannelLayout::Tag::indexMask) {
            channels.push_back(indexMask);
        }
    }
    return channels;
}

std::optional<DeviceProfile> getDeviceProfile(
        const ::aidl::android::media::audio::common::AudioDevice& audioDevice, bool isInput) {
    if (audioDevice.address.getTag() != AudioDeviceAddress::Tag::alsa) {
        LOG(ERROR) << __func__ << ": not alsa address: " << audioDevice.toString();
        return std::nullopt;
    }
    auto& alsaAddress = audioDevice.address.get<AudioDeviceAddress::Tag::alsa>();
    if (alsaAddress.size() != 2 || alsaAddress[0] < 0 || alsaAddress[1] < 0) {
        LOG(ERROR) << __func__
                   << ": malformed alsa address: " << ::android::internal::ToString(alsaAddress);
        return std::nullopt;
    }
    return DeviceProfile{.card = alsaAddress[0],
                         .device = alsaAddress[1],
                         .direction = isInput ? PCM_IN : PCM_OUT,
                         .isExternal = !audioDevice.type.connection.empty()};
}

std::optional<DeviceProfile> getDeviceProfile(
        const ::aidl::android::media::audio::common::AudioPort& audioPort) {
    if (audioPort.ext.getTag() != AudioPortExt::Tag::device) {
        LOG(ERROR) << __func__ << ": port id " << audioPort.id << " is not a device port";
        return std::nullopt;
    }
    auto& devicePort = audioPort.ext.get<AudioPortExt::Tag::device>();
    return getDeviceProfile(devicePort.device, audioPort.flags.getTag() == AudioIoFlags::input);
}

std::optional<struct pcm_config> getPcmConfig(const StreamContext& context, bool isInput) {
    struct pcm_config config;
    config.channels = alsa::getChannelCountFromChannelMask(context.getChannelLayout(), isInput);
    if (config.channels == 0) {
        LOG(ERROR) << __func__ << ": invalid channel=" << context.getChannelLayout().toString();
        return std::nullopt;
    }
    config.format = alsa::aidl2c_AudioFormatDescription_pcm_format(context.getFormat());
    if (config.format == PCM_FORMAT_INVALID) {
        LOG(ERROR) << __func__ << ": invalid format=" << context.getFormat().toString();
        return std::nullopt;
    }
    config.rate = context.getSampleRate();
    if (config.rate == 0) {
        LOG(ERROR) << __func__ << ": invalid sample rate=" << config.rate;
        return std::nullopt;
    }
    return config;
}

std::vector<int> getSampleRatesFromProfile(const alsa_device_profile* profile) {
    std::vector<int> sampleRates;
    for (int i = 0; i < std::min(MAX_PROFILE_SAMPLE_RATES, AUDIO_PORT_MAX_SAMPLING_RATES) &&
                    profile->sample_rates[i] != 0;
         i++) {
        sampleRates.push_back(profile->sample_rates[i]);
    }
    return sampleRates;
}

DeviceProxy openProxyForAttachedDevice(const DeviceProfile& deviceProfile,
                                       struct pcm_config* pcmConfig, size_t bufferFrameCount) {
    if (deviceProfile.isExternal) {
        LOG(FATAL) << __func__ << ": called for an external device, address=" << deviceProfile;
    }
    DeviceProxy proxy(deviceProfile);
    if (!profile_fill_builtin_device_info(proxy.getProfile(), pcmConfig, bufferFrameCount)) {
        LOG(FATAL) << __func__ << ": failed to init for built-in device, address=" << deviceProfile;
    }
    if (int err = proxy_prepare_from_default_config(proxy.get(), proxy.getProfile()); err != 0) {
        LOG(FATAL) << __func__ << ": fail to prepare for device address=" << deviceProfile
                   << " error=" << err;
        return DeviceProxy();
    }
    if (int err = proxy_open(proxy.get()); err != 0) {
        LOG(ERROR) << __func__ << ": failed to open device, address=" << deviceProfile
                   << " error=" << err;
        return DeviceProxy();
    }
    return proxy;
}

DeviceProxy openProxyForExternalDevice(const DeviceProfile& deviceProfile,
                                       struct pcm_config* pcmConfig, bool requireExactMatch) {
    if (!deviceProfile.isExternal) {
        LOG(FATAL) << __func__ << ": called for an attached device, address=" << deviceProfile;
    }
    auto proxy = readAlsaDeviceInfo(deviceProfile);
    if (proxy.get() == nullptr) {
        return proxy;
    }
    if (int err = proxy_prepare(proxy.get(), proxy.getProfile(), pcmConfig, requireExactMatch);
        err != 0) {
        LOG(ERROR) << __func__ << ": fail to prepare for device address=" << deviceProfile
                   << " error=" << err;
        return DeviceProxy();
    }
    if (int err = proxy_open(proxy.get()); err != 0) {
        LOG(ERROR) << __func__ << ": failed to open device, address=" << deviceProfile
                   << " error=" << err;
        return DeviceProxy();
    }
    return proxy;
}

DeviceProxy readAlsaDeviceInfo(const DeviceProfile& deviceProfile) {
    DeviceProxy proxy(deviceProfile);
    if (!profile_read_device_info(proxy.getProfile())) {
        LOG(ERROR) << __func__ << ": unable to read device info, device address=" << deviceProfile;
        return DeviceProxy();
    }
    return proxy;
}

void resetTransferredFrames(DeviceProxy& proxy, uint64_t frames) {
    if (proxy.get() != nullptr) {
        proxy.get()->transferred = frames;
    }
}

AudioFormatDescription c2aidl_pcm_format_AudioFormatDescription(enum pcm_format legacy) {
    return findValueOrDefault(getPcmFormatToAudioFormatDescMap(), legacy, AudioFormatDescription());
}

pcm_format aidl2c_AudioFormatDescription_pcm_format(const AudioFormatDescription& aidl) {
    return findValueOrDefault(getAudioFormatDescriptorToPcmFormatMap(), aidl, PCM_FORMAT_INVALID);
}

void applyGain(void* buffer, float gain, size_t bufferSizeBytes, enum pcm_format pcmFormat,
               int channelCount) {
    if (channelCount != 1 && channelCount != 2) {
        LOG(WARNING) << __func__ << ": unsupported channel count " << channelCount;
        return;
    }
    if (!getPcmFormatToAudioFormatDescMap().contains(pcmFormat)) {
        LOG(WARNING) << __func__ << ": unsupported pcm format " << pcmFormat;
        return;
    }
    if (std::abs(gain - kUnityGainFloat) < 1e-6) {
        return;
    }
    switch (pcmFormat) {
        case PCM_FORMAT_S16_LE:
            applyGainToInt16Buffer(buffer, bufferSizeBytes, gain, channelCount);
            break;
        case PCM_FORMAT_FLOAT_LE: {
            float* floatBuffer = (float*)buffer;
            applyGainToFloatBuffer(floatBuffer, bufferSizeBytes, gain);
        } break;
        case PCM_FORMAT_S24_LE:
            // PCM_FORMAT_S24_LE buffer is composed of signed fixed-point 32-bit Q8.23 data with
            // min and max limits of the same bit representation as min and max limits of
            // PCM_FORMAT_S32_LE buffer.
        case PCM_FORMAT_S32_LE: {
            int32_t* typedBuffer = (int32_t*)buffer;
            applyGainToInt32Buffer(typedBuffer, bufferSizeBytes, gain);
        } break;
        case PCM_FORMAT_S24_3LE: {
            int numSamples = bufferSizeBytes / (sizeof(uint8_t) * 3);
            if (numSamples == 0) {
                return;
            }
            std::unique_ptr<int32_t[]> typedBuffer(new int32_t[numSamples]);
            memcpy_to_i32_from_p24(typedBuffer.get(), (uint8_t*)buffer, numSamples);
            applyGainToInt32Buffer(typedBuffer.get(), numSamples * sizeof(int32_t), gain);
            memcpy_to_p24_from_i32((uint8_t*)buffer, typedBuffer.get(), numSamples);
        } break;
        default:
            LOG(FATAL) << __func__ << ": unsupported pcm format " << pcmFormat;
            break;
    }
}

}  // namespace aidl::android::hardware::audio::core::alsa
