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
#include <aidl/Gtest.h>
#include <aidl/Vintf.h>
#include <aidl/android/hardware/bluetooth/audio/BnBluetoothAudioPort.h>
#include <aidl/android/hardware/bluetooth/audio/IBluetoothAudioPort.h>
#include <aidl/android/hardware/bluetooth/audio/IBluetoothAudioProviderFactory.h>
#include <android/binder_auto_utils.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <fmq/AidlMessageQueue.h>

#include <cstdint>
#include <future>
#include <unordered_set>
#include <vector>

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::hardware::bluetooth::audio::A2dpConfiguration;
using aidl::android::hardware::bluetooth::audio::A2dpConfigurationHint;
using aidl::android::hardware::bluetooth::audio::A2dpRemoteCapabilities;
using aidl::android::hardware::bluetooth::audio::A2dpStatus;
using aidl::android::hardware::bluetooth::audio::A2dpStreamConfiguration;
using aidl::android::hardware::bluetooth::audio::AacCapabilities;
using aidl::android::hardware::bluetooth::audio::AacConfiguration;
using aidl::android::hardware::bluetooth::audio::AptxAdaptiveLeCapabilities;
using aidl::android::hardware::bluetooth::audio::AptxAdaptiveLeConfiguration;
using aidl::android::hardware::bluetooth::audio::AptxCapabilities;
using aidl::android::hardware::bluetooth::audio::AptxConfiguration;
using aidl::android::hardware::bluetooth::audio::AudioCapabilities;
using aidl::android::hardware::bluetooth::audio::AudioConfiguration;
using aidl::android::hardware::bluetooth::audio::AudioContext;
using aidl::android::hardware::bluetooth::audio::BnBluetoothAudioPort;
using aidl::android::hardware::bluetooth::audio::BroadcastCapability;
using aidl::android::hardware::bluetooth::audio::ChannelMode;
using aidl::android::hardware::bluetooth::audio::CodecCapabilities;
using aidl::android::hardware::bluetooth::audio::CodecConfiguration;
using aidl::android::hardware::bluetooth::audio::CodecId;
using aidl::android::hardware::bluetooth::audio::CodecInfo;
using aidl::android::hardware::bluetooth::audio::CodecParameters;
using aidl::android::hardware::bluetooth::audio::CodecSpecificCapabilitiesLtv;
using aidl::android::hardware::bluetooth::audio::CodecSpecificConfigurationLtv;
using aidl::android::hardware::bluetooth::audio::CodecType;
using aidl::android::hardware::bluetooth::audio::ConfigurationFlags;
using aidl::android::hardware::bluetooth::audio::HfpConfiguration;
using aidl::android::hardware::bluetooth::audio::IBluetoothAudioPort;
using aidl::android::hardware::bluetooth::audio::IBluetoothAudioProvider;
using aidl::android::hardware::bluetooth::audio::IBluetoothAudioProviderFactory;
using aidl::android::hardware::bluetooth::audio::LatencyMode;
using aidl::android::hardware::bluetooth::audio::Lc3Capabilities;
using aidl::android::hardware::bluetooth::audio::Lc3Configuration;
using aidl::android::hardware::bluetooth::audio::LdacCapabilities;
using aidl::android::hardware::bluetooth::audio::LdacConfiguration;
using aidl::android::hardware::bluetooth::audio::LeAudioAseConfiguration;
using aidl::android::hardware::bluetooth::audio::LeAudioBisConfiguration;
using aidl::android::hardware::bluetooth::audio::LeAudioBroadcastConfiguration;
using aidl::android::hardware::bluetooth::audio::
    LeAudioCodecCapabilitiesSetting;
using aidl::android::hardware::bluetooth::audio::LeAudioCodecConfiguration;
using aidl::android::hardware::bluetooth::audio::LeAudioConfiguration;
using aidl::android::hardware::bluetooth::audio::MetadataLtv;
using aidl::android::hardware::bluetooth::audio::OpusCapabilities;
using aidl::android::hardware::bluetooth::audio::OpusConfiguration;
using aidl::android::hardware::bluetooth::audio::PcmConfiguration;
using aidl::android::hardware::bluetooth::audio::PresentationPosition;
using aidl::android::hardware::bluetooth::audio::SbcAllocMethod;
using aidl::android::hardware::bluetooth::audio::SbcCapabilities;
using aidl::android::hardware::bluetooth::audio::SbcChannelMode;
using aidl::android::hardware::bluetooth::audio::SbcConfiguration;
using aidl::android::hardware::bluetooth::audio::SessionType;
using aidl::android::hardware::bluetooth::audio::UnicastCapability;
using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using android::AidlMessageQueue;
using android::ProcessState;
using android::String16;
using ndk::ScopedAStatus;
using ndk::SpAIBinder;

using MqDataType = int8_t;
using MqDataMode = SynchronizedReadWrite;
using DataMQ = AidlMessageQueue<MqDataType, MqDataMode>;
using DataMQDesc = MQDescriptor<MqDataType, MqDataMode>;

using LeAudioAseConfigurationSetting =
    IBluetoothAudioProvider::LeAudioAseConfigurationSetting;
using AseDirectionRequirement = IBluetoothAudioProvider::
    LeAudioConfigurationRequirement::AseDirectionRequirement;
using AseDirectionConfiguration = IBluetoothAudioProvider::
    LeAudioAseConfigurationSetting::AseDirectionConfiguration;
using AseQosDirectionRequirement = IBluetoothAudioProvider::
    LeAudioAseQosConfigurationRequirement::AseQosDirectionRequirement;
using LeAudioAseQosConfigurationRequirement =
    IBluetoothAudioProvider::LeAudioAseQosConfigurationRequirement;
using LeAudioAseQosConfiguration =
    IBluetoothAudioProvider::LeAudioAseQosConfiguration;
using LeAudioDeviceCapabilities =
    IBluetoothAudioProvider::LeAudioDeviceCapabilities;
using LeAudioConfigurationRequirement =
    IBluetoothAudioProvider::LeAudioConfigurationRequirement;
using LeAudioBroadcastConfigurationRequirement =
    IBluetoothAudioProvider::LeAudioBroadcastConfigurationRequirement;
using LeAudioBroadcastSubgroupConfiguration =
    IBluetoothAudioProvider::LeAudioBroadcastSubgroupConfiguration;
using LeAudioBroadcastSubgroupConfigurationRequirement =
    IBluetoothAudioProvider::LeAudioBroadcastSubgroupConfigurationRequirement;
using LeAudioBroadcastConfigurationSetting =
    IBluetoothAudioProvider::LeAudioBroadcastConfigurationSetting;
using LeAudioSubgroupBisConfiguration =
    IBluetoothAudioProvider::LeAudioSubgroupBisConfiguration;

// Constants

static constexpr int32_t a2dp_sample_rates[] = {0, 44100, 48000, 88200, 96000};
static constexpr int8_t a2dp_bits_per_samples[] = {0, 16, 24, 32};
static constexpr ChannelMode a2dp_channel_modes[] = {
    ChannelMode::UNKNOWN, ChannelMode::MONO, ChannelMode::STEREO};
static std::vector<LatencyMode> latency_modes = {LatencyMode::FREE};

enum class BluetoothAudioHalVersion : int32_t {
  VERSION_UNAVAILABLE = 0,
  VERSION_2_0,
  VERSION_2_1,
  VERSION_AIDL_V1,
  VERSION_AIDL_V2,
  VERSION_AIDL_V3,
  VERSION_AIDL_V4,
  VERSION_AIDL_V5,
};

// Some valid configs for HFP PCM configuration (software sessions)
static constexpr int32_t hfp_sample_rates_[] = {8000, 16000, 32000};
static constexpr int8_t hfp_bits_per_samples_[] = {16};
static constexpr ChannelMode hfp_channel_modes_[] = {ChannelMode::MONO};
static constexpr int32_t hfp_data_interval_us_[] = {7500};

// Helpers

template <typename T>
struct identity {
  typedef T type;
};

template <class T>
bool contained_in_vector(const std::vector<T>& vector,
                         const typename identity<T>::type& target) {
  return std::find(vector.begin(), vector.end(), target) != vector.end();
}

void copy_codec_specific(CodecConfiguration::CodecSpecific& dst,
                         const CodecConfiguration::CodecSpecific& src) {
  switch (src.getTag()) {
    case CodecConfiguration::CodecSpecific::sbcConfig:
      dst.set<CodecConfiguration::CodecSpecific::sbcConfig>(
          src.get<CodecConfiguration::CodecSpecific::sbcConfig>());
      break;
    case CodecConfiguration::CodecSpecific::aacConfig:
      dst.set<CodecConfiguration::CodecSpecific::aacConfig>(
          src.get<CodecConfiguration::CodecSpecific::aacConfig>());
      break;
    case CodecConfiguration::CodecSpecific::ldacConfig:
      dst.set<CodecConfiguration::CodecSpecific::ldacConfig>(
          src.get<CodecConfiguration::CodecSpecific::ldacConfig>());
      break;
    case CodecConfiguration::CodecSpecific::aptxConfig:
      dst.set<CodecConfiguration::CodecSpecific::aptxConfig>(
          src.get<CodecConfiguration::CodecSpecific::aptxConfig>());
      break;
    case CodecConfiguration::CodecSpecific::opusConfig:
      dst.set<CodecConfiguration::CodecSpecific::opusConfig>(
          src.get<CodecConfiguration::CodecSpecific::opusConfig>());
      break;
    case CodecConfiguration::CodecSpecific::aptxAdaptiveConfig:
      dst.set<CodecConfiguration::CodecSpecific::aptxAdaptiveConfig>(
          src.get<CodecConfiguration::CodecSpecific::aptxAdaptiveConfig>());
      break;
    default:
      break;
  }
}

static std::optional<CodecSpecificConfigurationLtv> GetConfigurationLtv(
    const std::vector<CodecSpecificConfigurationLtv>& configurationLtvs,
    CodecSpecificConfigurationLtv::Tag tag) {
  for (const auto ltv : configurationLtvs) {
    if (ltv.getTag() == tag) {
      return ltv;
    }
  }
  return std::nullopt;
}

class BluetoothAudioPort : public BnBluetoothAudioPort {
 public:
  BluetoothAudioPort() {}

  ndk::ScopedAStatus startStream(bool) { return ScopedAStatus::ok(); }

  ndk::ScopedAStatus suspendStream() { return ScopedAStatus::ok(); }

  ndk::ScopedAStatus stopStream() { return ScopedAStatus::ok(); }

  ndk::ScopedAStatus getPresentationPosition(PresentationPosition*) {
    return ScopedAStatus::ok();
  }

  ndk::ScopedAStatus updateSourceMetadata(const SourceMetadata&) {
    return ScopedAStatus::ok();
  }

  ndk::ScopedAStatus updateSinkMetadata(const SinkMetadata&) {
    return ScopedAStatus::ok();
  }

  ndk::ScopedAStatus setLatencyMode(const LatencyMode) {
    return ScopedAStatus::ok();
  }

  ndk::ScopedAStatus setCodecType(const CodecType) {
    return ScopedAStatus::ok();
  }

 protected:
  virtual ~BluetoothAudioPort() = default;
};

class BluetoothAudioProviderFactoryAidl
    : public testing::TestWithParam<std::string> {
 public:
  virtual void SetUp() override {
    provider_factory_ = IBluetoothAudioProviderFactory::fromBinder(
        SpAIBinder(AServiceManager_getService(GetParam().c_str())));
    audio_provider_ = nullptr;
    ASSERT_NE(provider_factory_, nullptr);
  }

  virtual void TearDown() override { provider_factory_ = nullptr; }

  void GetProviderInfoHelper(const SessionType& session_type) {
    temp_provider_info_ = std::nullopt;
    auto aidl_reval =
        provider_factory_->getProviderInfo(session_type, &temp_provider_info_);
  }

  void GetProviderCapabilitiesHelper(const SessionType& session_type) {
    temp_provider_capabilities_.clear();
    auto aidl_retval = provider_factory_->getProviderCapabilities(
        session_type, &temp_provider_capabilities_);
    // AIDL calls should not be failed and callback has to be executed
    ASSERT_TRUE(aidl_retval.isOk());
    switch (session_type) {
      case SessionType::UNKNOWN: {
        ASSERT_TRUE(temp_provider_capabilities_.empty());
      } break;
      case SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH:
      case SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH:
      case SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH:
      case SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH:
      case SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH:
      case SessionType::HFP_SOFTWARE_ENCODING_DATAPATH: {
        // All software paths are mandatory and must have exact 1
        // "PcmParameters"
        ASSERT_EQ(temp_provider_capabilities_.size(), 1);
        ASSERT_EQ(temp_provider_capabilities_[0].getTag(),
                  AudioCapabilities::pcmCapabilities);
      } break;
      case SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH:
      case SessionType::A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH: {
        std::unordered_set<CodecType> codec_types;
        // empty capability means offload is unsupported
        for (auto& audio_capability : temp_provider_capabilities_) {
          ASSERT_EQ(audio_capability.getTag(),
                    AudioCapabilities::a2dpCapabilities);
          const auto& codec_capabilities =
              audio_capability.get<AudioCapabilities::a2dpCapabilities>();
          // Every codec can present once at most
          ASSERT_EQ(codec_types.count(codec_capabilities.codecType), 0);
          switch (codec_capabilities.codecType) {
            case CodecType::SBC:
              ASSERT_EQ(codec_capabilities.capabilities.getTag(),
                        CodecCapabilities::Capabilities::sbcCapabilities);
              break;
            case CodecType::AAC:
              ASSERT_EQ(codec_capabilities.capabilities.getTag(),
                        CodecCapabilities::Capabilities::aacCapabilities);
              break;
            case CodecType::APTX:
            case CodecType::APTX_HD:
              ASSERT_EQ(codec_capabilities.capabilities.getTag(),
                        CodecCapabilities::Capabilities::aptxCapabilities);
              break;
            case CodecType::LDAC:
              ASSERT_EQ(codec_capabilities.capabilities.getTag(),
                        CodecCapabilities::Capabilities::ldacCapabilities);
              break;
            case CodecType::OPUS:
              ASSERT_EQ(codec_capabilities.capabilities.getTag(),
                        CodecCapabilities::Capabilities::opusCapabilities);
              break;
            case CodecType::APTX_ADAPTIVE:
            case CodecType::APTX_ADAPTIVE_LE:
            case CodecType::APTX_ADAPTIVE_LEX:
            case CodecType::LC3:
            case CodecType::VENDOR:
            case CodecType::UNKNOWN:
              break;
          }
          codec_types.insert(codec_capabilities.codecType);
        }
      } break;
      case SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH:
      case SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH:
      case SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH: {
        // empty capability means offload is unsupported since capabilities are
        // not hardcoded
        for (auto audio_capability : temp_provider_capabilities_) {
          ASSERT_EQ(audio_capability.getTag(),
                    AudioCapabilities::leAudioCapabilities);
        }
      } break;
      case SessionType::A2DP_SOFTWARE_DECODING_DATAPATH:
      case SessionType::HFP_SOFTWARE_DECODING_DATAPATH: {
        if (!temp_provider_capabilities_.empty()) {
          ASSERT_EQ(temp_provider_capabilities_.size(), 1);
          ASSERT_EQ(temp_provider_capabilities_[0].getTag(),
                    AudioCapabilities::pcmCapabilities);
        }
      } break;
      default: {
        ASSERT_TRUE(temp_provider_capabilities_.empty());
      }
    }
  }

  /***
   * This helps to open the specified provider and check the openProvider()
   * has corruct return values. BUT, to keep it simple, it does not consider
   * the capability, and please do so at the SetUp of each session's test.
   ***/
  void OpenProviderHelper(const SessionType& session_type) {
    auto aidl_retval =
        provider_factory_->openProvider(session_type, &audio_provider_);
    if (aidl_retval.isOk()) {
      ASSERT_NE(session_type, SessionType::UNKNOWN);
      ASSERT_NE(audio_provider_, nullptr);
      audio_port_ = ndk::SharedRefBase::make<BluetoothAudioPort>();
    } else {
      // optional session type
      ASSERT_TRUE(
          session_type == SessionType::UNKNOWN ||
          session_type ==
              SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
          session_type ==
              SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH ||
          session_type ==
              SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
          session_type ==
              SessionType::
                  LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
          session_type ==
              SessionType::A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH ||
          session_type == SessionType::A2DP_SOFTWARE_DECODING_DATAPATH ||
          session_type == SessionType::HFP_HARDWARE_OFFLOAD_DATAPATH ||
          session_type == SessionType::HFP_SOFTWARE_DECODING_DATAPATH ||
          session_type == SessionType::HFP_SOFTWARE_ENCODING_DATAPATH);
      ASSERT_EQ(audio_provider_, nullptr);
    }
  }

  void GetA2dpOffloadCapabilityHelper(const CodecType& codec_type) {
    temp_codec_capabilities_ = nullptr;
    for (auto& codec_capability : temp_provider_capabilities_) {
      auto& a2dp_capabilities =
          codec_capability.get<AudioCapabilities::a2dpCapabilities>();
      if (a2dp_capabilities.codecType != codec_type) {
        continue;
      }
      temp_codec_capabilities_ = &a2dp_capabilities;
    }
  }

  std::vector<CodecConfiguration::CodecSpecific>
  GetSbcCodecSpecificSupportedList(bool supported) {
    std::vector<CodecConfiguration::CodecSpecific> sbc_codec_specifics;
    if (!supported) {
      SbcConfiguration sbc_config{.sampleRateHz = 0, .bitsPerSample = 0};
      sbc_codec_specifics.push_back(
          CodecConfiguration::CodecSpecific(sbc_config));
      return sbc_codec_specifics;
    }
    GetA2dpOffloadCapabilityHelper(CodecType::SBC);
    if (temp_codec_capabilities_ == nullptr ||
        temp_codec_capabilities_->codecType != CodecType::SBC) {
      return sbc_codec_specifics;
    }
    // parse the capability
    auto& sbc_capability =
        temp_codec_capabilities_->capabilities
            .get<CodecCapabilities::Capabilities::sbcCapabilities>();
    if (sbc_capability.minBitpool > sbc_capability.maxBitpool) {
      return sbc_codec_specifics;
    }

    // combine those parameters into one list of
    // CodecConfiguration::CodecSpecific
    for (int32_t sample_rate : sbc_capability.sampleRateHz) {
      for (int8_t block_length : sbc_capability.blockLength) {
        for (int8_t num_subbands : sbc_capability.numSubbands) {
          for (int8_t bits_per_sample : sbc_capability.bitsPerSample) {
            for (auto channel_mode : sbc_capability.channelMode) {
              for (auto alloc_method : sbc_capability.allocMethod) {
                SbcConfiguration sbc_data = {
                    .sampleRateHz = sample_rate,
                    .channelMode = channel_mode,
                    .blockLength = block_length,
                    .numSubbands = num_subbands,
                    .allocMethod = alloc_method,
                    .bitsPerSample = bits_per_sample,
                    .minBitpool = sbc_capability.minBitpool,
                    .maxBitpool = sbc_capability.maxBitpool};
                sbc_codec_specifics.push_back(
                    CodecConfiguration::CodecSpecific(sbc_data));
              }
            }
          }
        }
      }
    }
    return sbc_codec_specifics;
  }

  std::vector<CodecConfiguration::CodecSpecific>
  GetAacCodecSpecificSupportedList(bool supported) {
    std::vector<CodecConfiguration::CodecSpecific> aac_codec_specifics;
    if (!supported) {
      AacConfiguration aac_config{.sampleRateHz = 0, .bitsPerSample = 0};
      aac_codec_specifics.push_back(
          CodecConfiguration::CodecSpecific(aac_config));
      return aac_codec_specifics;
    }
    GetA2dpOffloadCapabilityHelper(CodecType::AAC);
    if (temp_codec_capabilities_ == nullptr ||
        temp_codec_capabilities_->codecType != CodecType::AAC) {
      return aac_codec_specifics;
    }
    // parse the capability
    auto& aac_capability =
        temp_codec_capabilities_->capabilities
            .get<CodecCapabilities::Capabilities::aacCapabilities>();

    std::vector<bool> variable_bit_rate_enableds = {false};
    if (aac_capability.variableBitRateSupported) {
      variable_bit_rate_enableds.push_back(true);
    }

    std::vector<bool> adaptive_bit_rate_supporteds = {false};
    if (aac_capability.adaptiveBitRateSupported) {
      adaptive_bit_rate_supporteds.push_back(true);
    }

    // combine those parameters into one list of
    // CodecConfiguration::CodecSpecific
    for (auto object_type : aac_capability.objectType) {
      for (int32_t sample_rate : aac_capability.sampleRateHz) {
        for (auto channel_mode : aac_capability.channelMode) {
          for (int8_t bits_per_sample : aac_capability.bitsPerSample) {
            for (auto variable_bit_rate_enabled : variable_bit_rate_enableds) {
              for (auto adaptive_bit_rate_supported :
                   adaptive_bit_rate_supporteds) {
                AacConfiguration aac_data{
                    .objectType = object_type,
                    .sampleRateHz = sample_rate,
                    .channelMode = channel_mode,
                    .variableBitRateEnabled = variable_bit_rate_enabled,
                    .bitsPerSample = bits_per_sample,
                    .adaptiveBitRateSupported = adaptive_bit_rate_supported};
                aac_codec_specifics.push_back(
                    CodecConfiguration::CodecSpecific(aac_data));
              }
            }
          }
        }
      }
    }
    return aac_codec_specifics;
  }

  std::vector<CodecConfiguration::CodecSpecific>
  GetLdacCodecSpecificSupportedList(bool supported) {
    std::vector<CodecConfiguration::CodecSpecific> ldac_codec_specifics;
    if (!supported) {
      LdacConfiguration ldac_config{.sampleRateHz = 0, .bitsPerSample = 0};
      ldac_codec_specifics.push_back(
          CodecConfiguration::CodecSpecific(ldac_config));
      return ldac_codec_specifics;
    }
    GetA2dpOffloadCapabilityHelper(CodecType::LDAC);
    if (temp_codec_capabilities_ == nullptr ||
        temp_codec_capabilities_->codecType != CodecType::LDAC) {
      return ldac_codec_specifics;
    }
    // parse the capability
    auto& ldac_capability =
        temp_codec_capabilities_->capabilities
            .get<CodecCapabilities::Capabilities::ldacCapabilities>();

    // combine those parameters into one list of
    // CodecConfiguration::CodecSpecific
    for (int32_t sample_rate : ldac_capability.sampleRateHz) {
      for (int8_t bits_per_sample : ldac_capability.bitsPerSample) {
        for (auto channel_mode : ldac_capability.channelMode) {
          for (auto quality_index : ldac_capability.qualityIndex) {
            LdacConfiguration ldac_data{.sampleRateHz = sample_rate,
                                        .channelMode = channel_mode,
                                        .qualityIndex = quality_index,
                                        .bitsPerSample = bits_per_sample};
            ldac_codec_specifics.push_back(
                CodecConfiguration::CodecSpecific(ldac_data));
          }
        }
      }
    }
    return ldac_codec_specifics;
  }

  std::vector<CodecConfiguration::CodecSpecific>
  GetAptxCodecSpecificSupportedList(bool is_hd, bool supported) {
    std::vector<CodecConfiguration::CodecSpecific> aptx_codec_specifics;
    if (!supported) {
      AptxConfiguration aptx_config{.sampleRateHz = 0, .bitsPerSample = 0};
      aptx_codec_specifics.push_back(
          CodecConfiguration::CodecSpecific(aptx_config));
      return aptx_codec_specifics;
    }
    GetA2dpOffloadCapabilityHelper(
        (is_hd ? CodecType::APTX_HD : CodecType::APTX));
    if (temp_codec_capabilities_ == nullptr) {
      return aptx_codec_specifics;
    }
    if ((is_hd && temp_codec_capabilities_->codecType != CodecType::APTX_HD) ||
        (!is_hd && temp_codec_capabilities_->codecType != CodecType::APTX)) {
      return aptx_codec_specifics;
    }

    // parse the capability
    auto& aptx_capability =
        temp_codec_capabilities_->capabilities
            .get<CodecCapabilities::Capabilities::aptxCapabilities>();

    // combine those parameters into one list of
    // CodecConfiguration::CodecSpecific
    for (int8_t bits_per_sample : aptx_capability.bitsPerSample) {
      for (int32_t sample_rate : aptx_capability.sampleRateHz) {
        for (auto channel_mode : aptx_capability.channelMode) {
          AptxConfiguration aptx_data{.sampleRateHz = sample_rate,
                                      .channelMode = channel_mode,
                                      .bitsPerSample = bits_per_sample};
          aptx_codec_specifics.push_back(
              CodecConfiguration::CodecSpecific(aptx_data));
        }
      }
    }
    return aptx_codec_specifics;
  }

  std::vector<CodecConfiguration::CodecSpecific>
  GetOpusCodecSpecificSupportedList(bool supported) {
    std::vector<CodecConfiguration::CodecSpecific> opus_codec_specifics;
    if (!supported) {
      OpusConfiguration opus_config{.samplingFrequencyHz = 0,
                                    .frameDurationUs = 0};
      opus_codec_specifics.push_back(
          CodecConfiguration::CodecSpecific(opus_config));
      return opus_codec_specifics;
    }
    GetA2dpOffloadCapabilityHelper(CodecType::OPUS);
    if (temp_codec_capabilities_ == nullptr ||
        temp_codec_capabilities_->codecType != CodecType::OPUS) {
      return opus_codec_specifics;
    }
    // parse the capability
    auto& opus_capability =
        temp_codec_capabilities_->capabilities
            .get<CodecCapabilities::Capabilities::opusCapabilities>();

    // combine those parameters into one list of
    // CodecConfiguration::CodecSpecific
    for (int32_t samplingFrequencyHz : opus_capability->samplingFrequencyHz) {
      for (int32_t frameDurationUs : opus_capability->frameDurationUs) {
        for (auto channel_mode : opus_capability->channelMode) {
          OpusConfiguration opus_data{
              .samplingFrequencyHz = samplingFrequencyHz,
              .frameDurationUs = frameDurationUs,
              .channelMode = channel_mode,
          };
          opus_codec_specifics.push_back(
              CodecConfiguration::CodecSpecific(opus_data));
        }
      }
    }
    return opus_codec_specifics;
  }

  bool IsPcmConfigSupported(const PcmConfiguration& pcm_config) {
    if (temp_provider_capabilities_.size() != 1 ||
        temp_provider_capabilities_[0].getTag() !=
            AudioCapabilities::pcmCapabilities) {
      return false;
    }
    auto pcm_capability = temp_provider_capabilities_[0]
                              .get<AudioCapabilities::pcmCapabilities>();
    return (contained_in_vector(pcm_capability.channelMode,
                                pcm_config.channelMode) &&
            contained_in_vector(pcm_capability.sampleRateHz,
                                pcm_config.sampleRateHz) &&
            contained_in_vector(pcm_capability.bitsPerSample,
                                pcm_config.bitsPerSample));
  }

  std::shared_ptr<IBluetoothAudioProviderFactory> provider_factory_;
  std::shared_ptr<IBluetoothAudioProvider> audio_provider_;
  std::shared_ptr<IBluetoothAudioPort> audio_port_;
  std::vector<AudioCapabilities> temp_provider_capabilities_;
  std::optional<IBluetoothAudioProviderFactory::ProviderInfo>
      temp_provider_info_;

  // temp storage saves the specified codec capability by
  // GetOffloadCodecCapabilityHelper()
  CodecCapabilities* temp_codec_capabilities_;

  static constexpr SessionType kSessionTypes[] = {
      SessionType::UNKNOWN,
      SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH,
      SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH,
      SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH,
      SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::A2DP_SOFTWARE_DECODING_DATAPATH,
      SessionType::A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH,
  };

  static constexpr SessionType kAndroidVSessionType[] = {
      SessionType::HFP_SOFTWARE_ENCODING_DATAPATH,
      SessionType::HFP_SOFTWARE_DECODING_DATAPATH,
  };

  BluetoothAudioHalVersion GetProviderFactoryInterfaceVersion() {
    int32_t aidl_version = 0;
    if (provider_factory_ == nullptr) {
      return BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
    }

    auto aidl_retval = provider_factory_->getInterfaceVersion(&aidl_version);
    if (!aidl_retval.isOk()) {
      return BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
    }
    switch (aidl_version) {
      case 1:
        return BluetoothAudioHalVersion::VERSION_AIDL_V1;
      case 2:
        return BluetoothAudioHalVersion::VERSION_AIDL_V2;
      case 3:
        return BluetoothAudioHalVersion::VERSION_AIDL_V3;
      case 4:
        return BluetoothAudioHalVersion::VERSION_AIDL_V4;
      case 5:
        return BluetoothAudioHalVersion::VERSION_AIDL_V5;
      default:
        return BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
    }

    return BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
  }
};

/**
 * Test whether we can get the FactoryService from HIDL
 */
TEST_P(BluetoothAudioProviderFactoryAidl, GetProviderFactoryService) {}

/**
 * Test whether we can open a provider for each provider returned by
 * getProviderCapabilities() with non-empty capabalities
 */
TEST_P(BluetoothAudioProviderFactoryAidl,
       OpenProviderAndCheckCapabilitiesBySession) {
  for (auto session_type : kSessionTypes) {
    GetProviderCapabilitiesHelper(session_type);
    OpenProviderHelper(session_type);
    // We must be able to open a provider if its getProviderCapabilities()
    // returns non-empty list.
    EXPECT_TRUE(temp_provider_capabilities_.empty() ||
                audio_provider_ != nullptr);
  }
  if (GetProviderFactoryInterfaceVersion() >=
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    for (auto session_type : kAndroidVSessionType) {
      GetProviderCapabilitiesHelper(session_type);
      OpenProviderHelper(session_type);
      EXPECT_TRUE(temp_provider_capabilities_.empty() ||
                  audio_provider_ != nullptr);
    }
  }
}

/**
 * Test that getProviderInfo, when implemented,
 * returns empty information for session types for
 * software data paths.
 */
TEST_P(BluetoothAudioProviderFactoryAidl, getProviderInfo_invalidSessionTypes) {
  static constexpr SessionType kInvalidSessionTypes[]{
      SessionType::UNKNOWN,
      SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH,
      SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH,
      SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH,
      SessionType::A2DP_SOFTWARE_DECODING_DATAPATH,
  };

  for (auto session_type : kInvalidSessionTypes) {
    std::optional<IBluetoothAudioProviderFactory::ProviderInfo> provider_info =
        std::nullopt;
    auto aidl_retval =
        provider_factory_->getProviderInfo(session_type, &provider_info);
    if (!aidl_retval.isOk()) {
      continue;
    }

    // If getProviderInfo is supported, the provider info
    // must be empty for software session types.
    ASSERT_FALSE(provider_info.has_value());
  }
}

/**
 * Test that getProviderInfo, when implemented,
 * returns valid information for session types for
 * a2dp hardware data paths.
 */
TEST_P(BluetoothAudioProviderFactoryAidl, getProviderInfo_a2dpSessionTypes) {
  static constexpr SessionType kA2dpSessionTypes[]{
      SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH,
  };

  for (auto session_type : kA2dpSessionTypes) {
    std::optional<IBluetoothAudioProviderFactory::ProviderInfo> provider_info =
        std::nullopt;
    auto aidl_retval =
        provider_factory_->getProviderInfo(session_type, &provider_info);
    if (!aidl_retval.isOk() || !provider_info.has_value()) {
      continue;
    }

    for (auto const& codec_info : provider_info->codecInfos) {
      // The codec id must not be core.
      ASSERT_NE(codec_info.id.getTag(), CodecId::core);
      // The codec info must contain the information
      // for a2dp transport.
      ASSERT_EQ(codec_info.transport.getTag(), CodecInfo::Transport::a2dp);
    }
  }
}

/**
 * Test that getProviderInfo, when implemented,
 * returns valid information for session types for
 * le audio hardware data paths.
 */
TEST_P(BluetoothAudioProviderFactoryAidl, getProviderInfo_leAudioSessionTypes) {
  static constexpr SessionType kLeAudioSessionTypes[]{
      SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH,
      SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
  };

  for (auto session_type : kLeAudioSessionTypes) {
    std::optional<IBluetoothAudioProviderFactory::ProviderInfo> provider_info =
        std::nullopt;
    auto aidl_retval =
        provider_factory_->getProviderInfo(session_type, &provider_info);
    if (!aidl_retval.isOk() || !provider_info.has_value()) {
      continue;
    }

    for (auto const& codec_info : provider_info->codecInfos) {
      // The codec id must not be a2dp.
      ASSERT_NE(codec_info.id.getTag(), CodecId::a2dp);
      // The codec info must contain the information
      // for le audio transport.
      ASSERT_EQ(codec_info.transport.getTag(), CodecInfo::Transport::leAudio);
    }
  }
}

class BluetoothAudioProviderAidl : public BluetoothAudioProviderFactoryAidl {
 protected:
  std::optional<IBluetoothAudioProviderFactory::ProviderInfo>
      a2dp_encoding_provider_info_{};
  std::optional<IBluetoothAudioProviderFactory::ProviderInfo>
      a2dp_decoding_provider_info_{};
  std::shared_ptr<IBluetoothAudioProvider> a2dp_encoding_provider_{nullptr};
  std::shared_ptr<IBluetoothAudioProvider> a2dp_decoding_provider_{nullptr};

 public:
  void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    audio_port_ = ndk::SharedRefBase::make<BluetoothAudioPort>();

    (void)provider_factory_->getProviderInfo(
        SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
        &a2dp_encoding_provider_info_);

    (void)provider_factory_->getProviderInfo(
        SessionType::A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH,
        &a2dp_decoding_provider_info_);

    (void)provider_factory_->openProvider(
        SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
        &a2dp_encoding_provider_);

    (void)provider_factory_->openProvider(
        SessionType::A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH,
        &a2dp_decoding_provider_);
  }
};

/**
 * Calling parseA2dpConfiguration on a session of a different type than
 * A2DP_HARDWARE_OFFLOAD_(ENCODING|DECODING)_DATAPATH must fail.
 */
TEST_P(BluetoothAudioProviderAidl, parseA2dpConfiguration_invalidSessionType) {
  static constexpr SessionType kInvalidSessionTypes[] = {
      SessionType::UNKNOWN,
      SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH,
      SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH,
      SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH,
      SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::A2DP_SOFTWARE_DECODING_DATAPATH,
  };

  for (auto session_type : kInvalidSessionTypes) {
    // Open a BluetoothAudioProvider instance of the selected session type.
    // Skip validation if the provider cannot be opened.
    std::shared_ptr<IBluetoothAudioProvider> provider{nullptr};
    (void)provider_factory_->openProvider(session_type, &provider);
    if (provider == nullptr) {
      continue;
    }

    // parseA2dpConfiguration must fail without returning an A2dpStatus.
    CodecId codec_id(CodecId::A2dp::SBC);
    CodecParameters codec_parameters;
    A2dpStatus a2dp_status = A2dpStatus::OK;
    auto aidl_retval = provider->parseA2dpConfiguration(
        codec_id, std::vector<uint8_t>{}, &codec_parameters, &a2dp_status);
    EXPECT_FALSE(aidl_retval.isOk());
  }
}

/**
 * Calling parseA2dpConfiguration with an unknown codec must fail
 * with the A2dpStatus code INVALID_CODEC_TYPE or NOT_SUPPORTED_CODEC_TYPE.
 */
TEST_P(BluetoothAudioProviderAidl,
       parseA2dpConfiguration_unsupportedCodecType) {
  CodecId unsupported_core_id(CodecId::Core::CVSD);
  CodecId unsupported_vendor_id(
      CodecId::Vendor(0xFCB1, 0x42));  // Google Codec #42

  for (auto& provider : {a2dp_encoding_provider_, a2dp_decoding_provider_}) {
    if (provider == nullptr) {
      continue;
    }

    CodecParameters codec_parameters;
    A2dpStatus a2dp_status = A2dpStatus::OK;
    ::ndk::ScopedAStatus aidl_retval;

    // Test with two invalid codec identifiers: vendor or core.
    aidl_retval = provider->parseA2dpConfiguration(
        unsupported_core_id, std::vector<uint8_t>{}, &codec_parameters,
        &a2dp_status);
    EXPECT_TRUE(!aidl_retval.isOk() ||
                a2dp_status == A2dpStatus::NOT_SUPPORTED_CODEC_TYPE);

    aidl_retval = provider->parseA2dpConfiguration(
        unsupported_vendor_id, std::vector<uint8_t>{}, &codec_parameters,
        &a2dp_status);
    EXPECT_TRUE(!aidl_retval.isOk() ||
                a2dp_status == A2dpStatus::NOT_SUPPORTED_CODEC_TYPE);
  }
}

/**
 * Calling parseA2dpConfiguration with a known codec and invalid configuration
 * must fail with an A2dpStatus code different from INVALID_CODEC_TYPE or
 * NOT_SUPPORTED_CODEC_TYPE.
 */
TEST_P(BluetoothAudioProviderAidl,
       parseA2dpConfiguration_invalidConfiguration) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    CodecParameters codec_parameters;
    A2dpStatus a2dp_status = A2dpStatus::OK;
    ::ndk::ScopedAStatus aidl_retval;

    // Test with the first available codec in the provider info for testing.
    // The test runs with an empty parameters array, anything more specific
    // would need understanding the codec.
    aidl_retval = provider->parseA2dpConfiguration(
        provider_info->codecInfos[0].id, std::vector<uint8_t>{},
        &codec_parameters, &a2dp_status);
    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(a2dp_status != A2dpStatus::OK &&
                a2dp_status != A2dpStatus::NOT_SUPPORTED_CODEC_TYPE &&
                a2dp_status != A2dpStatus::INVALID_CODEC_TYPE);
  }
}

/**
 * Calling parseA2dpConfiguration with a known codec and valid parameters
 * must return with A2dpStatus OK.
 */
TEST_P(BluetoothAudioProviderAidl, parseA2dpConfiguration_valid) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    CodecParameters codec_parameters;
    A2dpStatus a2dp_status = A2dpStatus::OK;
    ::ndk::ScopedAStatus aidl_retval;

    // Test with the first available codec in the provider info for testing.
    // To get a valid configuration (the capabilities array in the provider
    // info is not a selection), getA2dpConfiguration is used with the
    // selected codec parameters as input.
    auto const& codec_info = provider_info->codecInfos[0];
    auto transport = codec_info.transport.get<CodecInfo::Transport::a2dp>();
    A2dpRemoteCapabilities remote_capabilities(/*seid*/ 0, codec_info.id,
                                               transport.capabilities);
    std::optional<A2dpConfiguration> configuration;
    aidl_retval = provider->getA2dpConfiguration(
        std::vector<A2dpRemoteCapabilities>{remote_capabilities},
        A2dpConfigurationHint(), &configuration);
    ASSERT_TRUE(aidl_retval.isOk());
    ASSERT_TRUE(configuration.has_value());

    aidl_retval = provider->parseA2dpConfiguration(
        configuration->id, configuration->configuration, &codec_parameters,
        &a2dp_status);
    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(a2dp_status == A2dpStatus::OK);
    EXPECT_EQ(codec_parameters, configuration->parameters);
  }
}

/**
 * Calling getA2dpConfiguration on a session of a different type than
 * A2DP_HARDWARE_OFFLOAD_(ENCODING|DECODING)_DATAPATH must fail.
 */
TEST_P(BluetoothAudioProviderAidl, getA2dpConfiguration_invalidSessionType) {
  static constexpr SessionType kInvalidSessionTypes[] = {
      SessionType::UNKNOWN,
      SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH,
      SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH,
      SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH,
      SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH,
      SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH,
      SessionType::A2DP_SOFTWARE_DECODING_DATAPATH,
  };

  for (auto session_type : kInvalidSessionTypes) {
    // Open a BluetoothAudioProvider instance of the selected session type.
    // Skip validation if the provider cannot be opened.
    std::shared_ptr<IBluetoothAudioProvider> provider{nullptr};
    auto aidl_retval = provider_factory_->openProvider(session_type, &provider);
    if (provider == nullptr) {
      continue;
    }

    // getA2dpConfiguration must fail without returning a configuration.
    std::optional<A2dpConfiguration> configuration;
    aidl_retval =
        provider->getA2dpConfiguration(std::vector<A2dpRemoteCapabilities>{},
                                       A2dpConfigurationHint(), &configuration);
    EXPECT_FALSE(aidl_retval.isOk());
  }
}

/**
 * Calling getA2dpConfiguration with empty or unknown remote capabilities
 * must return an empty configuration.
 */
TEST_P(BluetoothAudioProviderAidl,
       getA2dpConfiguration_unknownRemoteCapabilities) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    std::optional<A2dpConfiguration> configuration;
    ::ndk::ScopedAStatus aidl_retval;

    // Test with empty remote capabilities.
    aidl_retval =
        provider->getA2dpConfiguration(std::vector<A2dpRemoteCapabilities>{},
                                       A2dpConfigurationHint(), &configuration);
    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_FALSE(configuration.has_value());

    // Test with unknown remote capabilities.
    A2dpRemoteCapabilities unknown_core_remote_capabilities(
        /*seid*/ 0, CodecId::Core::CVSD, std::vector<uint8_t>{1, 2, 3});
    A2dpRemoteCapabilities unknown_vendor_remote_capabilities(
        /*seid*/ 1,
        /* Google Codec #42 */ CodecId::Vendor(0xFCB1, 0x42),
        std::vector<uint8_t>{1, 2, 3});
    aidl_retval = provider->getA2dpConfiguration(
        std::vector<A2dpRemoteCapabilities>{
            unknown_core_remote_capabilities,
            unknown_vendor_remote_capabilities,
        },
        A2dpConfigurationHint(), &configuration);
    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_FALSE(configuration.has_value());
  }
}

/**
 * Calling getA2dpConfiguration with invalid remote capabilities
 * must return an empty configuration.
 */
TEST_P(BluetoothAudioProviderAidl,
       getA2dpConfiguration_invalidRemoteCapabilities) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    std::optional<A2dpConfiguration> configuration;
    ::ndk::ScopedAStatus aidl_retval;

    // Use the first available codec in the provider info for testing.
    // The capabilities are modified to make them invalid.
    auto const& codec_info = provider_info->codecInfos[0];
    auto transport = codec_info.transport.get<CodecInfo::Transport::a2dp>();
    std::vector<uint8_t> invalid_capabilities = transport.capabilities;
    invalid_capabilities.push_back(0x42);  // adding bytes should be invalid.
    aidl_retval = provider->getA2dpConfiguration(
        std::vector<A2dpRemoteCapabilities>{
            A2dpRemoteCapabilities(/*seid*/ 0, codec_info.id,
                                   std::vector<uint8_t>()),
            A2dpRemoteCapabilities(/*seid*/ 1, codec_info.id,
                                   invalid_capabilities),
        },
        A2dpConfigurationHint(), &configuration);
    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_FALSE(configuration.has_value());
  }
}

/**
 * Calling getA2dpConfiguration with valid remote capabilities
 * must return a valid configuration. The selected parameters must
 * be contained in the original capabilities. The returned configuration
 * must match the returned parameters. The returned SEID must match the
 * input SEID.
 */
TEST_P(BluetoothAudioProviderAidl,
       getA2dpConfiguration_validRemoteCapabilities) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    // Test with all available codecs in the provider info.
    for (auto const& codec_info : provider_info->codecInfos) {
      auto a2dp_info = codec_info.transport.get<CodecInfo::Transport::a2dp>();
      std::optional<A2dpConfiguration> configuration{};
      ::ndk::ScopedAStatus aidl_retval;

      aidl_retval = provider->getA2dpConfiguration(
          std::vector<A2dpRemoteCapabilities>{
              A2dpRemoteCapabilities(/*seid*/ 42, codec_info.id,
                                     a2dp_info.capabilities),
          },
          A2dpConfigurationHint(), &configuration);

      ASSERT_TRUE(aidl_retval.isOk());
      ASSERT_TRUE(configuration.has_value());

      // Returned configuration must have the same codec id
      // as the remote capability.
      EXPECT_EQ(configuration->id, codec_info.id);

      // Returned configuration must have the same SEID
      // as the remote capability.
      EXPECT_EQ(configuration->remoteSeid, 42);

      // Returned codec parameters must be in the range of input
      // parameters.
      EXPECT_NE(
          std::find(a2dp_info.channelMode.begin(), a2dp_info.channelMode.end(),
                    configuration->parameters.channelMode),
          a2dp_info.channelMode.end());
      EXPECT_NE(std::find(a2dp_info.samplingFrequencyHz.begin(),
                          a2dp_info.samplingFrequencyHz.end(),
                          configuration->parameters.samplingFrequencyHz),
                a2dp_info.samplingFrequencyHz.end());
      EXPECT_NE(std::find(a2dp_info.bitdepth.begin(), a2dp_info.bitdepth.end(),
                          configuration->parameters.bitdepth),
                a2dp_info.bitdepth.end());
      EXPECT_EQ(a2dp_info.lossless, configuration->parameters.lossless);
      EXPECT_TRUE(configuration->parameters.minBitrate <=
                  configuration->parameters.maxBitrate);

      // Returned configuration must be parsable by parseA2dpParameters
      // and match the codec parameters.
      CodecParameters codec_parameters;
      A2dpStatus a2dp_status = A2dpStatus::OK;
      aidl_retval = provider->parseA2dpConfiguration(
          configuration->id, configuration->configuration, &codec_parameters,
          &a2dp_status);
      ASSERT_TRUE(aidl_retval.isOk());
      EXPECT_TRUE(a2dp_status == A2dpStatus::OK);
      EXPECT_EQ(codec_parameters, configuration->parameters);
    }
  }
}

/**
 * Calling getA2dpConfiguration with valid remote capabilities
 * with various hinted codec ids.
 */
TEST_P(BluetoothAudioProviderAidl, getA2dpConfiguration_hintCodecId) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    // Build the remote capabilities with all supported codecs.
    std::vector<A2dpRemoteCapabilities> remote_capabilities;
    for (size_t n = 0; n < provider_info->codecInfos.size(); n++) {
      auto const& codec_info = provider_info->codecInfos[n];
      auto a2dp_info = codec_info.transport.get<CodecInfo::Transport::a2dp>();
      remote_capabilities.push_back(A2dpRemoteCapabilities(
          /*seid*/ n, codec_info.id, a2dp_info.capabilities));
    }

    // Test with all supported codec identifiers,
    for (auto const& codec_info : provider_info->codecInfos) {
      std::optional<A2dpConfiguration> configuration{};
      ::ndk::ScopedAStatus aidl_retval;

      A2dpConfigurationHint hint;
      hint.codecId = codec_info.id;

      aidl_retval = provider->getA2dpConfiguration(remote_capabilities, hint,
                                                   &configuration);

      ASSERT_TRUE(aidl_retval.isOk());
      ASSERT_TRUE(configuration.has_value());
      EXPECT_EQ(configuration->id, codec_info.id);
    }

    // Test with unknown codec identifiers: either core or vendor.
    for (auto& codec_id :
         {CodecId(CodecId::Core::CVSD),
          CodecId(CodecId::Vendor(0xFCB1, 0x42)) /*Google Codec #42*/}) {
      std::optional<A2dpConfiguration> configuration{};
      ::ndk::ScopedAStatus aidl_retval;

      A2dpConfigurationHint hint;
      hint.codecId = codec_id;

      aidl_retval = provider->getA2dpConfiguration(remote_capabilities, hint,
                                                   &configuration);

      ASSERT_TRUE(aidl_retval.isOk());
      ASSERT_TRUE(configuration.has_value());
      EXPECT_NE(configuration->id, codec_id);
    }
  }
}

/**
 * Calling getA2dpConfiguration with valid remote capabilities
 * with various hinted channel modes.
 */
TEST_P(BluetoothAudioProviderAidl, getA2dpConfiguration_hintChannelMode) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    // Test with all available codecs in the provider info.
    for (auto const& codec_info : provider_info->codecInfos) {
      auto a2dp_info = codec_info.transport.get<CodecInfo::Transport::a2dp>();
      std::optional<A2dpConfiguration> configuration{};
      ::ndk::ScopedAStatus aidl_retval;

      for (auto& channel_mode :
           {ChannelMode::STEREO, ChannelMode::MONO, ChannelMode::DUALMONO}) {
        // Add the hint for the channel mode.
        A2dpConfigurationHint hint;
        auto& codec_parameters = hint.codecParameters.emplace();
        codec_parameters.channelMode = channel_mode;

        aidl_retval = provider->getA2dpConfiguration(
            std::vector<A2dpRemoteCapabilities>{
                A2dpRemoteCapabilities(/*seid*/ 42, codec_info.id,
                                       a2dp_info.capabilities),
            },
            hint, &configuration);

        ASSERT_TRUE(aidl_retval.isOk());
        ASSERT_TRUE(configuration.has_value());

        // The hint must be ignored if the channel mode is not supported
        // by the codec, and applied otherwise.
        ASSERT_EQ(configuration->parameters.channelMode == channel_mode,
                  std::find(a2dp_info.channelMode.begin(),
                            a2dp_info.channelMode.end(),
                            channel_mode) != a2dp_info.channelMode.end());
      }
    }
  }
}

/**
 * Calling getA2dpConfiguration with valid remote capabilities
 * with various hinted sampling frequencies.
 */
TEST_P(BluetoothAudioProviderAidl,
       getA2dpConfiguration_hintSamplingFrequencyHz) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    // Test with all available codecs in the provider info.
    for (auto const& codec_info : provider_info->codecInfos) {
      auto a2dp_info = codec_info.transport.get<CodecInfo::Transport::a2dp>();
      std::optional<A2dpConfiguration> configuration{};
      ::ndk::ScopedAStatus aidl_retval;

      for (auto& sampling_frequency_hz : {
               0,
               1,
               8000,
               16000,
               24000,
               32000,
               44100,
               48000,
               88200,
               96000,
               176400,
               192000,
           }) {
        // Add the hint for the sampling frequency.
        A2dpConfigurationHint hint;
        auto& codec_parameters = hint.codecParameters.emplace();
        codec_parameters.samplingFrequencyHz = sampling_frequency_hz;

        aidl_retval = provider->getA2dpConfiguration(
            std::vector<A2dpRemoteCapabilities>{
                A2dpRemoteCapabilities(/*seid*/ 42, codec_info.id,
                                       a2dp_info.capabilities),
            },
            hint, &configuration);

        ASSERT_TRUE(aidl_retval.isOk());
        ASSERT_TRUE(configuration.has_value());

        // The hint must be ignored if the sampling frequency is not supported
        // by the codec, and applied otherwise.
        ASSERT_EQ(configuration->parameters.samplingFrequencyHz ==
                      sampling_frequency_hz,
                  std::find(a2dp_info.samplingFrequencyHz.begin(),
                            a2dp_info.samplingFrequencyHz.end(),
                            sampling_frequency_hz) !=
                      a2dp_info.samplingFrequencyHz.end());
      }
    }
  }
}

/**
 * Calling getA2dpConfiguration with valid remote capabilities
 * with various hinted sampling bit-depths.
 */
TEST_P(BluetoothAudioProviderAidl, getA2dpConfiguration_hintBitdepth) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    // Test with all available codecs in the provider info.
    for (auto const& codec_info : provider_info->codecInfos) {
      auto a2dp_info = codec_info.transport.get<CodecInfo::Transport::a2dp>();
      std::optional<A2dpConfiguration> configuration{};
      ::ndk::ScopedAStatus aidl_retval;

      for (auto& bitdepth : {0, 1, 16, 24, 32}) {
        // Add the hint for the bit depth.
        A2dpConfigurationHint hint;
        auto& codec_parameters = hint.codecParameters.emplace();
        codec_parameters.bitdepth = bitdepth;

        aidl_retval = provider->getA2dpConfiguration(
            std::vector<A2dpRemoteCapabilities>{
                A2dpRemoteCapabilities(/*seid*/ 42, codec_info.id,
                                       a2dp_info.capabilities),
            },
            hint, &configuration);

        ASSERT_TRUE(aidl_retval.isOk());
        ASSERT_TRUE(configuration.has_value());

        // The hint must be ignored if the bitdepth is not supported
        // by the codec, and applied otherwise.
        ASSERT_EQ(
            configuration->parameters.bitdepth == bitdepth,
            std::find(a2dp_info.bitdepth.begin(), a2dp_info.bitdepth.end(),
                      bitdepth) != a2dp_info.bitdepth.end());
      }
    }
  }
}

/**
 * Calling startSession with an unknown codec id must fail.
 */
TEST_P(BluetoothAudioProviderAidl, startSession_unknownCodecId) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    for (auto& codec_id :
         {CodecId(CodecId::Core::CVSD),
          CodecId(CodecId::Vendor(0xFCB1, 0x42) /*Google Codec #42*/)}) {
      A2dpStreamConfiguration a2dp_config;
      DataMQDesc data_mq_desc;

      a2dp_config.codecId = codec_id;
      a2dp_config.configuration = std::vector<uint8_t>{1, 2, 3};

      auto aidl_retval =
          provider->startSession(audio_port_, AudioConfiguration(a2dp_config),
                                 std::vector<LatencyMode>{}, &data_mq_desc);

      EXPECT_FALSE(aidl_retval.isOk());
    }
  }
}

/**
 * Calling startSession with a known codec and a valid configuration
 * must succeed.
 */
TEST_P(BluetoothAudioProviderAidl, startSession_valid) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    // Use the first available codec in the provider info for testing.
    // To get a valid configuration (the capabilities array in the provider
    // info is not a selection), getA2dpConfiguration is used with the
    // selected codec parameters as input.
    auto const& codec_info = provider_info->codecInfos[0];
    auto a2dp_info = codec_info.transport.get<CodecInfo::Transport::a2dp>();
    ::ndk::ScopedAStatus aidl_retval;
    A2dpRemoteCapabilities remote_capabilities(/*seid*/ 0, codec_info.id,
                                               a2dp_info.capabilities);
    std::optional<A2dpConfiguration> configuration;
    aidl_retval = provider->getA2dpConfiguration(
        std::vector<A2dpRemoteCapabilities>{remote_capabilities},
        A2dpConfigurationHint(), &configuration);
    ASSERT_TRUE(aidl_retval.isOk());
    ASSERT_TRUE(configuration.has_value());

    // Build the stream configuration.
    A2dpStreamConfiguration a2dp_config;
    DataMQDesc data_mq_desc;

    a2dp_config.codecId = codec_info.id;
    a2dp_config.configuration = configuration->configuration;

    aidl_retval =
        provider->startSession(audio_port_, AudioConfiguration(a2dp_config),
                               std::vector<LatencyMode>{}, &data_mq_desc);

    EXPECT_TRUE(aidl_retval.isOk());
  }
}

/**
 * Calling startSession with a known codec but an invalid configuration
 * must fail.
 */
TEST_P(BluetoothAudioProviderAidl, startSession_invalidConfiguration) {
  for (auto& [provider, provider_info] :
       {std::pair(a2dp_encoding_provider_, a2dp_encoding_provider_info_),
        std::pair(a2dp_decoding_provider_, a2dp_decoding_provider_info_)}) {
    if (provider == nullptr || !provider_info.has_value() ||
        provider_info->codecInfos.empty()) {
      continue;
    }

    // Use the first available codec in the provider info for testing.
    // To get a valid configuration (the capabilities array in the provider
    // info is not a selection), getA2dpConfiguration is used with the
    // selected codec parameters as input.
    ::ndk::ScopedAStatus aidl_retval;
    auto const& codec_info = provider_info->codecInfos[0];
    auto a2dp_info = codec_info.transport.get<CodecInfo::Transport::a2dp>();
    A2dpRemoteCapabilities remote_capabilities(/*seid*/ 0, codec_info.id,
                                               a2dp_info.capabilities);
    std::optional<A2dpConfiguration> configuration;
    aidl_retval = provider->getA2dpConfiguration(
        std::vector<A2dpRemoteCapabilities>{remote_capabilities},
        A2dpConfigurationHint(), &configuration);
    ASSERT_TRUE(aidl_retval.isOk());
    ASSERT_TRUE(configuration.has_value());

    // Build the stream configuration but edit the configuration bytes
    // to make it invalid.
    A2dpStreamConfiguration a2dp_config;
    DataMQDesc data_mq_desc;

    a2dp_config.codecId = codec_info.id;
    a2dp_config.configuration = configuration->configuration;
    a2dp_config.configuration.push_back(42);

    aidl_retval =
        provider->startSession(audio_port_, AudioConfiguration(a2dp_config),
                               std::vector<LatencyMode>{}, &data_mq_desc);

    EXPECT_FALSE(aidl_retval.isOk());
  }
}

/**
 * openProvider A2DP_SOFTWARE_ENCODING_DATAPATH
 */
class BluetoothAudioProviderA2dpEncodingSoftwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH);
    OpenProviderHelper(SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH);
    ASSERT_NE(audio_provider_, nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }
};

/**
 * Test whether we can open a provider of type
 */
TEST_P(BluetoothAudioProviderA2dpEncodingSoftwareAidl,
       OpenA2dpEncodingSoftwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH can be started and stopped
 * with different PCM config
 */
TEST_P(BluetoothAudioProviderA2dpEncodingSoftwareAidl,
       StartAndEndA2dpEncodingSoftwareSessionWithPossiblePcmConfig) {
  for (auto sample_rate : a2dp_sample_rates) {
    for (auto bits_per_sample : a2dp_bits_per_samples) {
      for (auto channel_mode : a2dp_channel_modes) {
        PcmConfiguration pcm_config{
            .sampleRateHz = sample_rate,
            .channelMode = channel_mode,
            .bitsPerSample = bits_per_sample,
        };
        bool is_codec_config_valid = IsPcmConfigSupported(pcm_config);
        DataMQDesc mq_desc;
        auto aidl_retval = audio_provider_->startSession(
            audio_port_, AudioConfiguration(pcm_config), latency_modes,
            &mq_desc);
        DataMQ data_mq(mq_desc);

        EXPECT_EQ(aidl_retval.isOk(), is_codec_config_valid);
        if (is_codec_config_valid) {
          EXPECT_TRUE(data_mq.isValid());
        }
        EXPECT_TRUE(audio_provider_->endSession().isOk());
      }
    }
  }
}

/**
 * openProvider HFP_SOFTWARE_ENCODING_DATAPATH
 */
class BluetoothAudioProviderHfpSoftwareEncodingAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    if (GetProviderFactoryInterfaceVersion() <
        BluetoothAudioHalVersion::VERSION_AIDL_V4) {
      GTEST_SKIP();
    }
    GetProviderCapabilitiesHelper(SessionType::HFP_SOFTWARE_ENCODING_DATAPATH);
    OpenProviderHelper(SessionType::HFP_SOFTWARE_ENCODING_DATAPATH);
    ASSERT_NE(audio_provider_, nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  bool OpenSession(int32_t sample_rate, int8_t bits_per_sample,
                   ChannelMode channel_mode, int32_t data_interval_us) {
    PcmConfiguration pcm_config{
        .sampleRateHz = sample_rate,
        .channelMode = channel_mode,
        .bitsPerSample = bits_per_sample,
        .dataIntervalUs = data_interval_us,
    };
    // Checking against provider capability from getProviderCapabilities
    // For HFP software, it's
    // BluetoothAudioCodecs::GetSoftwarePcmCapabilities();
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(pcm_config), latency_modes, &mq_desc);
    DataMQ data_mq(mq_desc);

    if (!aidl_retval.isOk()) return false;
    if (!data_mq.isValid()) return false;
    return true;
  }
};

/**
 * Test whether we can open a provider of type
 */
TEST_P(BluetoothAudioProviderHfpSoftwareEncodingAidl,
       OpenHfpSoftwareEncodingProvider) {}

/**
 * Test whether each provider of type
 * SessionType::HFP_SOFTWARE_ENCODING_DATAPATH can be started and stopped with
 * different PCM config
 */
TEST_P(BluetoothAudioProviderHfpSoftwareEncodingAidl,
       StartAndEndHfpEncodingSoftwareSessionWithPossiblePcmConfig) {
  for (auto sample_rate : hfp_sample_rates_) {
    for (auto bits_per_sample : hfp_bits_per_samples_) {
      for (auto channel_mode : hfp_channel_modes_) {
        for (auto data_interval_us : hfp_data_interval_us_) {
          EXPECT_TRUE(OpenSession(sample_rate, bits_per_sample, channel_mode,
                                  data_interval_us));
          EXPECT_TRUE(audio_provider_->endSession().isOk());
        }
      }
    }
  }
}

/**
 * openProvider HFP_SOFTWARE_DECODING_DATAPATH
 */
class BluetoothAudioProviderHfpSoftwareDecodingAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    if (GetProviderFactoryInterfaceVersion() <
        BluetoothAudioHalVersion::VERSION_AIDL_V4) {
      GTEST_SKIP();
    }
    GetProviderCapabilitiesHelper(SessionType::HFP_SOFTWARE_DECODING_DATAPATH);
    OpenProviderHelper(SessionType::HFP_SOFTWARE_DECODING_DATAPATH);
    ASSERT_NE(audio_provider_, nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  bool OpenSession(int32_t sample_rate, int8_t bits_per_sample,
                   ChannelMode channel_mode, int32_t data_interval_us) {
    PcmConfiguration pcm_config{
        .sampleRateHz = sample_rate,
        .channelMode = channel_mode,
        .bitsPerSample = bits_per_sample,
        .dataIntervalUs = data_interval_us,
    };
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(pcm_config), latency_modes, &mq_desc);
    DataMQ data_mq(mq_desc);

    if (!aidl_retval.isOk()) return false;
    if (!data_mq.isValid()) return false;
    return true;
  }
};

/**
 * Test whether we can open a provider of type
 */
TEST_P(BluetoothAudioProviderHfpSoftwareDecodingAidl,
       OpenHfpSoftwareDecodingProvider) {}

/**
 * Test whether each provider of type
 * SessionType::HFP_SOFTWARE_DECODING_DATAPATH can be started and stopped with
 * different PCM config
 */
TEST_P(BluetoothAudioProviderHfpSoftwareDecodingAidl,
       StartAndEndHfpDecodingSoftwareSessionWithPossiblePcmConfig) {
  for (auto sample_rate : hfp_sample_rates_) {
    for (auto bits_per_sample : hfp_bits_per_samples_) {
      for (auto channel_mode : hfp_channel_modes_) {
        for (auto data_interval_us : hfp_data_interval_us_) {
          EXPECT_TRUE(OpenSession(sample_rate, bits_per_sample, channel_mode,
                                  data_interval_us));
          EXPECT_TRUE(audio_provider_->endSession().isOk());
        }
      }
    }
  }
}

/**
 * openProvider A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH
 */
class BluetoothAudioProviderA2dpEncodingHardwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
    OpenProviderHelper(SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
    ASSERT_TRUE(temp_provider_capabilities_.empty() ||
                audio_provider_ != nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  bool IsOffloadSupported() { return (temp_provider_capabilities_.size() > 0); }
};

/**
 * Test whether we can open a provider of type
 */
TEST_P(BluetoothAudioProviderA2dpEncodingHardwareAidl,
       OpenA2dpEncodingHardwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_ENCODING_DATAPATH can be started and stopped
 * with SBC hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpEncodingHardwareAidl,
       StartAndEndA2dpSbcEncodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  CodecConfiguration codec_config = {
      .codecType = CodecType::SBC,
      .encodedAudioBitrate = 328000,
      .peerMtu = 1005,
      .isScmstEnabled = false,
  };
  auto sbc_codec_specifics = GetSbcCodecSpecificSupportedList(true);

  for (auto& codec_specific : sbc_codec_specifics) {
    copy_codec_specific(codec_config.config, codec_specific);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(codec_config), latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_ENCODING_DATAPATH can be started and stopped
 * with AAC hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpEncodingHardwareAidl,
       StartAndEndA2dpAacEncodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  CodecConfiguration codec_config = {
      .codecType = CodecType::AAC,
      .encodedAudioBitrate = 320000,
      .peerMtu = 1005,
      .isScmstEnabled = false,
  };
  auto aac_codec_specifics = GetAacCodecSpecificSupportedList(true);

  for (auto& codec_specific : aac_codec_specifics) {
    copy_codec_specific(codec_config.config, codec_specific);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(codec_config), latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_ENCODING_DATAPATH can be started and stopped
 * with LDAC hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpEncodingHardwareAidl,
       StartAndEndA2dpLdacEncodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  CodecConfiguration codec_config = {
      .codecType = CodecType::LDAC,
      .encodedAudioBitrate = 990000,
      .peerMtu = 1005,
      .isScmstEnabled = false,
  };
  auto ldac_codec_specifics = GetLdacCodecSpecificSupportedList(true);

  for (auto& codec_specific : ldac_codec_specifics) {
    copy_codec_specific(codec_config.config, codec_specific);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(codec_config), latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_ENCODING_DATAPATH can be started and stopped
 * with Opus hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpEncodingHardwareAidl,
       StartAndEndA2dpOpusEncodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  CodecConfiguration codec_config = {
      .codecType = CodecType::OPUS,
      .encodedAudioBitrate = 990000,
      .peerMtu = 1005,
      .isScmstEnabled = false,
  };
  auto opus_codec_specifics = GetOpusCodecSpecificSupportedList(true);

  for (auto& codec_specific : opus_codec_specifics) {
    copy_codec_specific(codec_config.config, codec_specific);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(codec_config), latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_ENCODING_DATAPATH can be started and stopped
 * with AptX hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpEncodingHardwareAidl,
       StartAndEndA2dpAptxEncodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  for (auto codec_type : {CodecType::APTX, CodecType::APTX_HD}) {
    CodecConfiguration codec_config = {
        .codecType = codec_type,
        .encodedAudioBitrate =
            (codec_type == CodecType::APTX ? 352000 : 576000),
        .peerMtu = 1005,
        .isScmstEnabled = false,
    };

    auto aptx_codec_specifics = GetAptxCodecSpecificSupportedList(
        (codec_type == CodecType::APTX_HD ? true : false), true);

    for (auto& codec_specific : aptx_codec_specifics) {
      copy_codec_specific(codec_config.config, codec_specific);
      DataMQDesc mq_desc;
      auto aidl_retval = audio_provider_->startSession(
          audio_port_, AudioConfiguration(codec_config), latency_modes,
          &mq_desc);

      ASSERT_TRUE(aidl_retval.isOk());
      EXPECT_TRUE(audio_provider_->endSession().isOk());
    }
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_ENCODING_DATAPATH can be started and stopped
 * with an invalid codec config
 */
TEST_P(BluetoothAudioProviderA2dpEncodingHardwareAidl,
       StartAndEndA2dpEncodingHardwareSessionInvalidCodecConfig) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }
  ASSERT_NE(audio_provider_, nullptr);

  std::vector<CodecConfiguration::CodecSpecific> codec_specifics;
  for (auto codec_type : ndk::enum_range<CodecType>()) {
    switch (codec_type) {
      case CodecType::SBC:
        codec_specifics = GetSbcCodecSpecificSupportedList(false);
        break;
      case CodecType::AAC:
        codec_specifics = GetAacCodecSpecificSupportedList(false);
        break;
      case CodecType::LDAC:
        codec_specifics = GetLdacCodecSpecificSupportedList(false);
        break;
      case CodecType::APTX:
        codec_specifics = GetAptxCodecSpecificSupportedList(false, false);
        break;
      case CodecType::APTX_HD:
        codec_specifics = GetAptxCodecSpecificSupportedList(true, false);
        break;
      case CodecType::OPUS:
        codec_specifics = GetOpusCodecSpecificSupportedList(false);
        continue;
      case CodecType::APTX_ADAPTIVE:
      case CodecType::APTX_ADAPTIVE_LE:
      case CodecType::APTX_ADAPTIVE_LEX:
      case CodecType::LC3:
      case CodecType::VENDOR:
      case CodecType::UNKNOWN:
        codec_specifics.clear();
        break;
    }
    if (codec_specifics.empty()) {
      continue;
    }

    CodecConfiguration codec_config = {
        .codecType = codec_type,
        .encodedAudioBitrate = 328000,
        .peerMtu = 1005,
        .isScmstEnabled = false,
    };
    for (auto codec_specific : codec_specifics) {
      copy_codec_specific(codec_config.config, codec_specific);
      DataMQDesc mq_desc;
      auto aidl_retval = audio_provider_->startSession(
          audio_port_, AudioConfiguration(codec_config), latency_modes,
          &mq_desc);

      // AIDL call should fail on invalid codec
      ASSERT_FALSE(aidl_retval.isOk());
      EXPECT_TRUE(audio_provider_->endSession().isOk());
    }
  }
}

/**
 * openProvider HFP_HARDWARE_OFFLOAD_DATAPATH
 */
class BluetoothAudioProviderHfpHardwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    if (GetProviderFactoryInterfaceVersion() <
        BluetoothAudioHalVersion::VERSION_AIDL_V4) {
      GTEST_SKIP();
    }
    GetProviderInfoHelper(SessionType::HFP_HARDWARE_OFFLOAD_DATAPATH);
    OpenProviderHelper(SessionType::HFP_HARDWARE_OFFLOAD_DATAPATH);
    // Can open or empty capability
    ASSERT_TRUE(temp_provider_capabilities_.empty() ||
                audio_provider_ != nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  bool OpenSession(CodecId codec_id, int connection_handle, bool nrec,
                   bool controller_codec) {
    // Check if can open session with a Hfp configuration
    HfpConfiguration hfp_configuration{
        .codecId = codec_id,
        .connectionHandle = connection_handle,
        .nrec = nrec,
        .controllerCodec = controller_codec,
    };
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(hfp_configuration), latency_modes,
        &mq_desc);

    // Only check if aidl is ok to start session.
    return aidl_retval.isOk();
  }
};

/**
 * Test whether we can open a provider of type
 */
TEST_P(BluetoothAudioProviderHfpHardwareAidl, OpenHfpHardwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::HFP_SOFTWARE_DECODING_DATAPATH can be started and stopped with
 * different HFP config
 */
TEST_P(BluetoothAudioProviderHfpHardwareAidl,
       StartAndEndHfpHardwareSessionWithPossiblePcmConfig) {
  // Try to open with a sample configuration
  EXPECT_TRUE(OpenSession(CodecId::Core::CVSD, 6, false, true));
  EXPECT_TRUE(audio_provider_->endSession().isOk());
}

/**
 * openProvider HEARING_AID_SOFTWARE_ENCODING_DATAPATH
 */
class BluetoothAudioProviderHearingAidSoftwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH);
    OpenProviderHelper(SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH);
    ASSERT_NE(audio_provider_, nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  static constexpr int32_t hearing_aid_sample_rates_[] = {0, 16000, 24000};
  static constexpr int8_t hearing_aid_bits_per_samples_[] = {0, 16, 24};
  static constexpr ChannelMode hearing_aid_channel_modes_[] = {
      ChannelMode::UNKNOWN, ChannelMode::MONO, ChannelMode::STEREO};
};

/**
 * Test whether we can open a provider of type
 */
TEST_P(BluetoothAudioProviderHearingAidSoftwareAidl,
       OpenHearingAidSoftwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH can be started and
 * stopped with different PCM config
 */
TEST_P(BluetoothAudioProviderHearingAidSoftwareAidl,
       StartAndEndHearingAidSessionWithPossiblePcmConfig) {
  for (int32_t sample_rate : hearing_aid_sample_rates_) {
    for (int8_t bits_per_sample : hearing_aid_bits_per_samples_) {
      for (auto channel_mode : hearing_aid_channel_modes_) {
        PcmConfiguration pcm_config{
            .sampleRateHz = sample_rate,
            .channelMode = channel_mode,
            .bitsPerSample = bits_per_sample,
        };
        bool is_codec_config_valid = IsPcmConfigSupported(pcm_config);
        DataMQDesc mq_desc;
        auto aidl_retval = audio_provider_->startSession(
            audio_port_, AudioConfiguration(pcm_config), latency_modes,
            &mq_desc);
        DataMQ data_mq(mq_desc);

        EXPECT_EQ(aidl_retval.isOk(), is_codec_config_valid);
        if (is_codec_config_valid) {
          EXPECT_TRUE(data_mq.isValid());
        }
        EXPECT_TRUE(audio_provider_->endSession().isOk());
      }
    }
  }
}

/**
 * openProvider LE_AUDIO_SOFTWARE_ENCODING_DATAPATH
 */
class BluetoothAudioProviderLeAudioOutputSoftwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH);
    OpenProviderHelper(SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH);
    ASSERT_NE(audio_provider_, nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  static constexpr int32_t le_audio_output_sample_rates_[] = {
      0, 8000, 16000, 24000, 32000, 44100, 48000,
  };
  static constexpr int8_t le_audio_output_bits_per_samples_[] = {0, 16, 24};
  static constexpr ChannelMode le_audio_output_channel_modes_[] = {
      ChannelMode::UNKNOWN, ChannelMode::MONO, ChannelMode::STEREO};
  static constexpr int32_t le_audio_output_data_interval_us_[] = {
      0 /* Invalid */, 10000 /* Valid 10ms */};
};

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH can be started and
 * stopped
 */
TEST_P(BluetoothAudioProviderLeAudioOutputSoftwareAidl,
       OpenLeAudioOutputSoftwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH can be started and
 * stopped with different PCM config
 */
TEST_P(BluetoothAudioProviderLeAudioOutputSoftwareAidl,
       StartAndEndLeAudioOutputSessionWithPossiblePcmConfig) {
  for (auto sample_rate : le_audio_output_sample_rates_) {
    for (auto bits_per_sample : le_audio_output_bits_per_samples_) {
      for (auto channel_mode : le_audio_output_channel_modes_) {
        for (auto data_interval_us : le_audio_output_data_interval_us_) {
          PcmConfiguration pcm_config{
              .sampleRateHz = sample_rate,
              .channelMode = channel_mode,
              .bitsPerSample = bits_per_sample,
              .dataIntervalUs = data_interval_us,
          };
          bool is_codec_config_valid =
              IsPcmConfigSupported(pcm_config) && pcm_config.dataIntervalUs > 0;
          DataMQDesc mq_desc;
          auto aidl_retval = audio_provider_->startSession(
              audio_port_, AudioConfiguration(pcm_config), latency_modes,
              &mq_desc);
          DataMQ data_mq(mq_desc);

          EXPECT_EQ(aidl_retval.isOk(), is_codec_config_valid);
          if (is_codec_config_valid) {
            EXPECT_TRUE(data_mq.isValid());
          }
          EXPECT_TRUE(audio_provider_->endSession().isOk());
        }
      }
    }
  }
}

/**
 * openProvider LE_AUDIO_SOFTWARE_DECODING_DATAPATH
 */
class BluetoothAudioProviderLeAudioInputSoftwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH);
    OpenProviderHelper(SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH);
    ASSERT_NE(audio_provider_, nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  static constexpr int32_t le_audio_input_sample_rates_[] = {
      0, 8000, 16000, 24000, 32000, 44100, 48000};
  static constexpr int8_t le_audio_input_bits_per_samples_[] = {0, 16, 24};
  static constexpr ChannelMode le_audio_input_channel_modes_[] = {
      ChannelMode::UNKNOWN, ChannelMode::MONO, ChannelMode::STEREO};
  static constexpr int32_t le_audio_input_data_interval_us_[] = {
      0 /* Invalid */, 10000 /* Valid 10ms */};
};

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH can be started and
 * stopped
 */
TEST_P(BluetoothAudioProviderLeAudioInputSoftwareAidl,
       OpenLeAudioInputSoftwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH can be started and
 * stopped with different PCM config
 */
TEST_P(BluetoothAudioProviderLeAudioInputSoftwareAidl,
       StartAndEndLeAudioInputSessionWithPossiblePcmConfig) {
  for (auto sample_rate : le_audio_input_sample_rates_) {
    for (auto bits_per_sample : le_audio_input_bits_per_samples_) {
      for (auto channel_mode : le_audio_input_channel_modes_) {
        for (auto data_interval_us : le_audio_input_data_interval_us_) {
          PcmConfiguration pcm_config{
              .sampleRateHz = sample_rate,
              .channelMode = channel_mode,
              .bitsPerSample = bits_per_sample,
              .dataIntervalUs = data_interval_us,
          };
          bool is_codec_config_valid =
              IsPcmConfigSupported(pcm_config) && pcm_config.dataIntervalUs > 0;
          DataMQDesc mq_desc;
          auto aidl_retval = audio_provider_->startSession(
              audio_port_, AudioConfiguration(pcm_config), latency_modes,
              &mq_desc);
          DataMQ data_mq(mq_desc);

          EXPECT_EQ(aidl_retval.isOk(), is_codec_config_valid);
          if (is_codec_config_valid) {
            EXPECT_TRUE(data_mq.isValid());
          }
          EXPECT_TRUE(audio_provider_->endSession().isOk());
        }
      }
    }
  }
}

/**
 * openProvider LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH
 */
class BluetoothAudioProviderLeAudioOutputHardwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
    GetProviderInfoHelper(
        SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
    OpenProviderHelper(
        SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
    ASSERT_TRUE(temp_provider_capabilities_.empty() ||
                audio_provider_ != nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  bool IsMultidirectionalCapabilitiesEnabled() {
    if (!temp_provider_info_.has_value()) return false;

    return temp_provider_info_.value().supportsMultidirectionalCapabilities;
  }

  bool IsAsymmetricConfigurationAllowed() {
    if (!temp_provider_info_.has_value()) return false;
    if (temp_provider_info_.value().codecInfos.empty()) return false;

    for (auto& codec_info : temp_provider_info_.value().codecInfos) {
      if (codec_info.transport.getTag() != CodecInfo::Transport::leAudio) {
        return false;
      }

      auto flags =
          codec_info.transport.get<CodecInfo::Transport::leAudio>().flags;

      if (!flags) {
        continue;
      }

      if (flags->bitmask &
          ConfigurationFlags::ALLOW_ASYMMETRIC_CONFIGURATIONS) {
        return true;
      }
    }

    return false;
  }

  bool IsOffloadOutputSupported() {
    for (auto& capability : temp_provider_capabilities_) {
      if (capability.getTag() != AudioCapabilities::leAudioCapabilities) {
        continue;
      }
      auto& le_audio_capability =
          capability.get<AudioCapabilities::leAudioCapabilities>();
      if (le_audio_capability.unicastEncodeCapability.codecType !=
          CodecType::UNKNOWN)
        return true;
    }
    return false;
  }

  bool IsOffloadOutputProviderInfoSupported() {
    if (!temp_provider_info_.has_value()) return false;
    if (temp_provider_info_.value().codecInfos.empty()) return false;
    // Check if all codec info is of LeAudio type
    for (auto& codec_info : temp_provider_info_.value().codecInfos) {
      if (codec_info.transport.getTag() != CodecInfo::Transport::leAudio)
        return false;
    }
    return true;
  }

  std::vector<Lc3Configuration> GetUnicastLc3SupportedListFromProviderInfo() {
    std::vector<Lc3Configuration> le_audio_codec_configs;
    for (auto& codec_info : temp_provider_info_.value().codecInfos) {
      // Only gets LC3 codec information
      if (codec_info.id != CodecId::Core::LC3) continue;
      // Combine those parameters into one list of Lc3Configuration
      auto& transport =
          codec_info.transport.get<CodecInfo::Transport::Tag::leAudio>();
      for (int32_t samplingFrequencyHz : transport.samplingFrequencyHz) {
        for (int32_t frameDurationUs : transport.frameDurationUs) {
          for (int32_t octetsPerFrame : transport.bitdepth) {
            Lc3Configuration lc3_config = {
                .samplingFrequencyHz = samplingFrequencyHz,
                .frameDurationUs = frameDurationUs,
                .octetsPerFrame = octetsPerFrame,
            };
            le_audio_codec_configs.push_back(lc3_config);
          }
        }
      }
    }

    return le_audio_codec_configs;
  }

  AudioContext GetAudioContext(int32_t bitmask) {
    AudioContext media_audio_context;
    media_audio_context.bitmask = bitmask;
    return media_audio_context;
  }

  LeAudioDeviceCapabilities GetDefaultRemoteSinkCapability() {
    // Create a capability
    LeAudioDeviceCapabilities capability;

    capability.codecId = CodecId::Core::LC3;

    auto pref_context_metadata = MetadataLtv::PreferredAudioContexts();
    pref_context_metadata.values =
        GetAudioContext(AudioContext::MEDIA | AudioContext::CONVERSATIONAL |
                        AudioContext::GAME);
    capability.metadata = {pref_context_metadata};

    auto sampling_rate =
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies();
    sampling_rate.bitmask =
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ16000 |
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ8000;
    auto frame_duration =
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations();
    frame_duration.bitmask =
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US7500 |
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US10000;
    auto octets = CodecSpecificCapabilitiesLtv::SupportedOctetsPerCodecFrame();
    octets.min = 0;
    octets.max = 120;
    auto frames = CodecSpecificCapabilitiesLtv::SupportedMaxCodecFramesPerSDU();
    frames.value = 2;
    capability.codecSpecificCapabilities = {sampling_rate, frame_duration,
                                            octets, frames};
    return capability;
  }

  LeAudioDeviceCapabilities GetOpusRemoteSinkCapability() {
    // Create a capability specifically for vendor OPUS
    LeAudioDeviceCapabilities capability;

    auto vendor_codec = CodecId::Vendor();
    vendor_codec.codecId = 255;
    vendor_codec.id = 224;
    capability.codecId = vendor_codec;

    auto pref_context_metadata = MetadataLtv::PreferredAudioContexts();
    pref_context_metadata.values =
        GetAudioContext(AudioContext::MEDIA | AudioContext::CONVERSATIONAL |
                        AudioContext::GAME);
    capability.metadata = {pref_context_metadata};

    auto sampling_rate =
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies();
    sampling_rate.bitmask =
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ16000 |
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ8000 |
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ48000;
    auto frame_duration =
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations();
    frame_duration.bitmask =
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US7500 |
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US10000 |
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US20000;
    auto octets = CodecSpecificCapabilitiesLtv::SupportedOctetsPerCodecFrame();
    octets.min = 0;
    octets.max = 240;
    auto frames = CodecSpecificCapabilitiesLtv::SupportedMaxCodecFramesPerSDU();
    frames.value = 2;
    capability.codecSpecificCapabilities = {sampling_rate, frame_duration,
                                            octets, frames};
    return capability;
  }

  LeAudioDeviceCapabilities GetDefaultRemoteSourceCapability() {
    // Create a capability
    LeAudioDeviceCapabilities capability;

    capability.codecId = CodecId::Core::LC3;

    auto pref_context_metadata = MetadataLtv::PreferredAudioContexts();
    pref_context_metadata.values =
        GetAudioContext(AudioContext::LIVE_AUDIO |
                        AudioContext::CONVERSATIONAL | AudioContext::GAME);
    capability.metadata = {pref_context_metadata};

    auto sampling_rate =
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies();
    sampling_rate.bitmask =
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ16000 |
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ8000;
    auto frame_duration =
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations();
    frame_duration.bitmask =
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US7500 |
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US10000;
    auto octets = CodecSpecificCapabilitiesLtv::SupportedOctetsPerCodecFrame();
    octets.min = 0;
    octets.max = 120;
    auto frames = CodecSpecificCapabilitiesLtv::SupportedMaxCodecFramesPerSDU();
    frames.value = 2;
    capability.codecSpecificCapabilities = {sampling_rate, frame_duration,
                                            octets, frames};
    return capability;
  }

  bool IsAseRequirementSatisfiedWithUnknownChannelCount(
      const std::vector<std::optional<AseDirectionRequirement>>&
          ase_requirements,
      const std::vector<std::optional<AseDirectionConfiguration>>&
          ase_configurations) {
    /* This is mandatory  to match sample freq, allocation however, when in the
     * device group there is only one device which supports left and right
     * allocation, and channel count is hidden from the BT stack, the BT stack
     * will send single requirement but it can receive two configurations if the
     * channel count is 1.
     */

    int num_of_ase_requirements = 0;
    for (const auto& ase_req : ase_requirements) {
      auto required_allocation_ltv = GetConfigurationLtv(
          ase_req->aseConfiguration.codecConfiguration,
          CodecSpecificConfigurationLtv::Tag::audioChannelAllocation);
      if (required_allocation_ltv == std::nullopt) {
        continue;
      }
      int required_allocation =
          required_allocation_ltv
              ->get<
                  CodecSpecificConfigurationLtv::Tag::audioChannelAllocation>()
              .bitmask;
      num_of_ase_requirements += std::bitset<32>(required_allocation).count();
    }

    int num_of_satisfied_ase_requirements = 0;
    for (const auto& ase_req : ase_requirements) {
      if (!ase_req) {
        continue;
      }
      auto required_sample_freq_ltv = GetConfigurationLtv(
          ase_req->aseConfiguration.codecConfiguration,
          CodecSpecificConfigurationLtv::Tag::samplingFrequency);
      auto required_allocation_ltv = GetConfigurationLtv(
          ase_req->aseConfiguration.codecConfiguration,
          CodecSpecificConfigurationLtv::Tag::audioChannelAllocation);

      /* Allocation and sample freq shall be always in the requirement */
      if (!required_sample_freq_ltv || !required_allocation_ltv) {
        return false;
      }

      int required_allocation =
          required_allocation_ltv
              ->get<
                  CodecSpecificConfigurationLtv::Tag::audioChannelAllocation>()
              .bitmask;

      for (const auto& ase_conf : ase_configurations) {
        if (!ase_conf) {
          continue;
        }
        auto config_sample_freq_ltv = GetConfigurationLtv(
            ase_conf->aseConfiguration.codecConfiguration,
            CodecSpecificConfigurationLtv::Tag::samplingFrequency);
        auto config_allocation_ltv = GetConfigurationLtv(
            ase_conf->aseConfiguration.codecConfiguration,
            CodecSpecificConfigurationLtv::Tag::audioChannelAllocation);
        if (config_sample_freq_ltv == std::nullopt ||
            config_allocation_ltv == std::nullopt) {
          return false;
        }

        int configured_allocation = config_allocation_ltv
                                        ->get<CodecSpecificConfigurationLtv::
                                                  Tag::audioChannelAllocation>()
                                        .bitmask;

        if (config_sample_freq_ltv == required_sample_freq_ltv &&
            (required_allocation & configured_allocation)) {
          num_of_satisfied_ase_requirements +=
              std::bitset<32>(configured_allocation).count();
        }
      }
    }

    return (num_of_satisfied_ase_requirements == num_of_ase_requirements);
  }

  bool IsAseRequirementSatisfied(
      const std::vector<std::optional<AseDirectionRequirement>>&
          ase_requirements,
      const std::vector<std::optional<AseDirectionConfiguration>>&
          ase_configurations) {
    // This is mandatory  to match sample freq, allocation
    int num_of_satisfied_ase_requirements = 0;

    int required_allocations = 0;
    for (const auto& ase_req : ase_requirements) {
      auto required_allocation_ltv = GetConfigurationLtv(
          ase_req->aseConfiguration.codecConfiguration,
          CodecSpecificConfigurationLtv::Tag::audioChannelAllocation);
      if (required_allocation_ltv == std::nullopt) {
        continue;
      }

      int allocations =
          required_allocation_ltv
              ->get<
                  CodecSpecificConfigurationLtv::Tag::audioChannelAllocation>()
              .bitmask;
      required_allocations += std::bitset<32>(allocations).count();
    }

    if (ase_requirements.size() != required_allocations) {
      /* If more than one allication is requested in the requirement, then use
       * different verifier */
      return IsAseRequirementSatisfiedWithUnknownChannelCount(
          ase_requirements, ase_configurations);
    }

    for (const auto& ase_req : ase_requirements) {
      if (!ase_req) {
        continue;
      }
      auto required_sample_freq_ltv = GetConfigurationLtv(
          ase_req->aseConfiguration.codecConfiguration,
          CodecSpecificConfigurationLtv::Tag::samplingFrequency);
      auto required_allocation_ltv = GetConfigurationLtv(
          ase_req->aseConfiguration.codecConfiguration,
          CodecSpecificConfigurationLtv::Tag::audioChannelAllocation);

      /* Allocation and sample freq shall be always in the requirement */
      if (!required_sample_freq_ltv || !required_allocation_ltv) {
        return false;
      }

      for (const auto& ase_conf : ase_configurations) {
        if (!ase_conf) {
          continue;
        }
        auto config_sample_freq_ltv = GetConfigurationLtv(
            ase_conf->aseConfiguration.codecConfiguration,
            CodecSpecificConfigurationLtv::Tag::samplingFrequency);
        auto config_allocation_ltv = GetConfigurationLtv(
            ase_conf->aseConfiguration.codecConfiguration,
            CodecSpecificConfigurationLtv::Tag::audioChannelAllocation);
        if (config_sample_freq_ltv == std::nullopt ||
            config_allocation_ltv == std::nullopt) {
          return false;
        }

        if (config_sample_freq_ltv == required_sample_freq_ltv &&
            config_allocation_ltv == required_allocation_ltv) {
          num_of_satisfied_ase_requirements++;
          break;
        }
      }
    }

    return (num_of_satisfied_ase_requirements == ase_requirements.size());
  }

  static void VerifyCodecParameters(
      ::aidl::android::hardware::bluetooth::audio::IBluetoothAudioProvider::
          LeAudioAseConfigurationSetting::AseDirectionConfiguration config) {
    ASSERT_NE(config.aseConfiguration.codecConfiguration.size(), 0lu);
    ASSERT_TRUE(config.qosConfiguration.has_value());

    int32_t frame_blocks = 1;  // by default 1 if not set
    int8_t frame_duration = 0;
    int32_t octets_per_frame = 0;
    std::bitset<32> allocation_bitmask = 0;

    for (auto const& param : config.aseConfiguration.codecConfiguration) {
      if (param.getTag() ==
          ::aidl::android::hardware::bluetooth::audio::
              CodecSpecificConfigurationLtv::Tag::codecFrameBlocksPerSDU) {
        frame_blocks = param
                           .get<::aidl::android::hardware::bluetooth::audio::
                                    CodecSpecificConfigurationLtv::Tag::
                                        codecFrameBlocksPerSDU>()
                           .value;
      } else if (param.getTag() ==
                 ::aidl::android::hardware::bluetooth::audio::
                     CodecSpecificConfigurationLtv::Tag::frameDuration) {
        frame_duration = static_cast<int8_t>(
            param.get<::aidl::android::hardware::bluetooth::audio::
                          CodecSpecificConfigurationLtv::Tag::frameDuration>());
      } else if (param.getTag() ==
                 ::aidl::android::hardware::bluetooth::audio::
                     CodecSpecificConfigurationLtv::Tag::octetsPerCodecFrame) {
        octets_per_frame = static_cast<int32_t>(
            param
                .get<::aidl::android::hardware::bluetooth::audio::
                         CodecSpecificConfigurationLtv::Tag::
                             octetsPerCodecFrame>()
                .value);
      } else if (param.getTag() == ::aidl::android::hardware::bluetooth::audio::
                                       CodecSpecificConfigurationLtv::Tag::
                                           audioChannelAllocation) {
        allocation_bitmask = static_cast<int32_t>(
            param
                .get<::aidl::android::hardware::bluetooth::audio::
                         CodecSpecificConfigurationLtv::Tag::
                             audioChannelAllocation>()
                .bitmask);
      }
    }

    ASSERT_NE(frame_blocks, 0);
    ASSERT_NE(frame_duration, 0);
    ASSERT_NE(octets_per_frame, 0);

    auto const num_channels_per_cis = allocation_bitmask.count();
    ASSERT_NE(num_channels_per_cis, 0);

    // Verify if QoS takes the codec frame blocks per SDU into the account
    ASSERT_TRUE(config.qosConfiguration->sduIntervalUs >=
                frame_blocks * frame_duration);
    ASSERT_TRUE(config.qosConfiguration->maxSdu >=
                (frame_blocks * num_channels_per_cis * octets_per_frame));
  }

  void VerifyIfRequirementsSatisfied(
      const std::vector<LeAudioConfigurationRequirement>& requirements,
      const std::vector<LeAudioAseConfigurationSetting>& configurations) {
    if (requirements.empty() && configurations.empty()) {
      return;
    }

    /* It might happen that vendor lib will provide same configuration for
     * multiple contexts and it should be accepted
     */

    int num_of_requirements = 0;
    for (const auto& req : requirements) {
      num_of_requirements += std::bitset<32>(req.audioContext.bitmask).count();
    }

    int num_of_configurations = 0;
    for (const auto& conf : configurations) {
      num_of_configurations +=
          std::bitset<32>(conf.audioContext.bitmask).count();
    }

    ASSERT_EQ(num_of_requirements, num_of_configurations);

    int num_of_satisfied_requirements = 0;
    for (const auto& req : requirements) {
      for (const auto& conf : configurations) {
        if ((req.audioContext.bitmask & conf.audioContext.bitmask) !=
            req.audioContext.bitmask) {
          continue;
        }

        bool sink_req_satisfied = false;
        if (req.sinkAseRequirement) {
          ASSERT_TRUE(conf.sinkAseConfiguration.has_value());
          sink_req_satisfied = IsAseRequirementSatisfied(
              *req.sinkAseRequirement, *conf.sinkAseConfiguration);

          ASSERT_NE(conf.sinkAseConfiguration->size(), 0lu);
          for (auto const& cfg : conf.sinkAseConfiguration.value()) {
            ASSERT_TRUE(cfg.has_value());
            VerifyCodecParameters(cfg.value());
          }
        }

        bool source_req_satisfied = false;
        if (req.sourceAseRequirement) {
          ASSERT_TRUE(conf.sourceAseConfiguration.has_value());
          source_req_satisfied = IsAseRequirementSatisfied(
              *req.sourceAseRequirement, *conf.sourceAseConfiguration);

          ASSERT_NE(conf.sourceAseConfiguration->size(), 0lu);
          for (auto const& cfg : conf.sourceAseConfiguration.value()) {
            ASSERT_TRUE(cfg.has_value());
            VerifyCodecParameters(cfg.value());
          }
        }

        if (req.sinkAseRequirement && req.sourceAseRequirement) {
          if (!conf.sinkAseConfiguration || !conf.sourceAseConfiguration) {
            continue;
          }

          if (!sink_req_satisfied || !source_req_satisfied) {
            continue;
          }
          num_of_satisfied_requirements +=
              std::bitset<32>(req.audioContext.bitmask).count();
          break;
        } else if (req.sinkAseRequirement) {
          if (!sink_req_satisfied) {
            continue;
          }
          num_of_satisfied_requirements +=
              std::bitset<32>(req.audioContext.bitmask).count();
          break;
        } else if (req.sourceAseRequirement) {
          if (!source_req_satisfied) {
            continue;
          }
          num_of_satisfied_requirements +=
              std::bitset<32>(req.audioContext.bitmask).count();
          break;
        }
      }
    }
    ASSERT_EQ(num_of_satisfied_requirements, num_of_requirements);
  }

  LeAudioConfigurationRequirement GetUnicastDefaultRequirement(
      int32_t context_bits, bool is_sink_requirement,
      bool is_source_requriement,
      CodecSpecificConfigurationLtv::SamplingFrequency freq =
          CodecSpecificConfigurationLtv::SamplingFrequency::HZ16000) {
    // Create a requirements
    LeAudioConfigurationRequirement requirement;
    requirement.audioContext = GetAudioContext(context_bits);

    auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
    allocation.bitmask =
        CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
        CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

    auto direction_ase_requriement = AseDirectionRequirement();
    direction_ase_requriement.aseConfiguration.codecId = CodecId::Core::LC3;
    direction_ase_requriement.aseConfiguration.targetLatency =
        LeAudioAseConfiguration::TargetLatency::BALANCED_LATENCY_RELIABILITY;

    direction_ase_requriement.aseConfiguration.codecConfiguration = {
        freq, CodecSpecificConfigurationLtv::FrameDuration::US10000, allocation

    };
    if (is_sink_requirement)
      requirement.sinkAseRequirement = {direction_ase_requriement};

    if (is_source_requriement)
      requirement.sourceAseRequirement = {direction_ase_requriement};

    return requirement;
  }

  LeAudioConfigurationRequirement GetOpusUnicastRequirement(
      int32_t context_bits, bool is_sink_requirement,
      bool is_source_requriement,
      CodecSpecificConfigurationLtv::SamplingFrequency freq =
          CodecSpecificConfigurationLtv::SamplingFrequency::HZ48000) {
    // Create a requirements
    LeAudioConfigurationRequirement requirement;
    requirement.audioContext = GetAudioContext(context_bits);

    auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
    allocation.bitmask =
        CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
        CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

    auto direction_ase_requriement = AseDirectionRequirement();
    auto vendor_codec = CodecId::Vendor();
    vendor_codec.codecId = 255;
    vendor_codec.id = 224;
    direction_ase_requriement.aseConfiguration.codecId = vendor_codec;
    direction_ase_requriement.aseConfiguration.targetLatency =
        LeAudioAseConfiguration::TargetLatency::HIGHER_RELIABILITY;

    direction_ase_requriement.aseConfiguration.codecConfiguration = {
        freq, CodecSpecificConfigurationLtv::FrameDuration::US20000, allocation

    };
    if (is_sink_requirement)
      requirement.sinkAseRequirement = {direction_ase_requriement};

    if (is_source_requriement)
      requirement.sourceAseRequirement = {direction_ase_requriement};

    return requirement;
  }

  LeAudioConfigurationRequirement GetUnicastGameRequirement(bool asymmetric) {
    // Create a requirements
    LeAudioConfigurationRequirement requirement;
    requirement.audioContext = GetAudioContext(AudioContext::GAME);

    auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
    allocation.bitmask =
        CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
        CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

    auto sink_ase_requriement = AseDirectionRequirement();
    sink_ase_requriement.aseConfiguration.codecId = CodecId::Core::LC3;
    sink_ase_requriement.aseConfiguration.targetLatency =
        LeAudioAseConfiguration::TargetLatency::BALANCED_LATENCY_RELIABILITY;

    sink_ase_requriement.aseConfiguration.codecConfiguration = {
        CodecSpecificConfigurationLtv::SamplingFrequency::HZ16000,
        CodecSpecificConfigurationLtv::FrameDuration::US10000, allocation};

    auto source_ase_requriement = AseDirectionRequirement();
    source_ase_requriement.aseConfiguration.codecId = CodecId::Core::LC3;
    source_ase_requriement.aseConfiguration.targetLatency =
        LeAudioAseConfiguration::TargetLatency::BALANCED_LATENCY_RELIABILITY;

    if (asymmetric) {
      source_ase_requriement.aseConfiguration.codecConfiguration = {
          CodecSpecificConfigurationLtv::SamplingFrequency::HZ16000,
          CodecSpecificConfigurationLtv::FrameDuration::US10000, allocation};
    } else {
      source_ase_requriement.aseConfiguration.codecConfiguration = {
          CodecSpecificConfigurationLtv::SamplingFrequency::HZ32000,
          CodecSpecificConfigurationLtv::FrameDuration::US10000, allocation};
    }

    requirement.sinkAseRequirement = {sink_ase_requriement};
    requirement.sourceAseRequirement = {source_ase_requriement};

    return requirement;
  }

  LeAudioAseQosConfigurationRequirement GetQosRequirements(
      bool is_sink_requirement, bool is_source_requriement, bool valid = true) {
    LeAudioAseQosConfigurationRequirement qosRequirement;

    auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
    allocation.bitmask =
        CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
        CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

    AseQosDirectionRequirement directionalRequirement = {
        .framing = IBluetoothAudioProvider::Framing::UNFRAMED,
        .preferredRetransmissionNum = 2,
        .maxTransportLatencyMs = 10,
        .presentationDelayMinUs = 40000,
        .presentationDelayMaxUs = 40000,
        .aseConfiguration =
            {
                .targetLatency = LeAudioAseConfiguration::TargetLatency::
                    BALANCED_LATENCY_RELIABILITY,
                .codecId = CodecId::Core::LC3,
                .codecConfiguration =
                    {CodecSpecificConfigurationLtv::SamplingFrequency::HZ16000,
                     CodecSpecificConfigurationLtv::FrameDuration::US10000,
                     allocation},
            },
    };

    if (!valid) {
      // clear some required values;
      directionalRequirement.maxTransportLatencyMs = 0;
      directionalRequirement.presentationDelayMaxUs = 0;
    }

    qosRequirement.sinkAseQosRequirement = directionalRequirement;
    if (is_source_requriement && is_sink_requirement) {
      qosRequirement.sourceAseQosRequirement = directionalRequirement;
      qosRequirement.sinkAseQosRequirement = directionalRequirement;
    } else if (is_source_requriement) {
      qosRequirement.sourceAseQosRequirement = directionalRequirement;
      qosRequirement.sinkAseQosRequirement = std::nullopt;
    } else if (is_sink_requirement) {
      qosRequirement.sourceAseQosRequirement = std::nullopt;
      qosRequirement.sinkAseQosRequirement = directionalRequirement;
    }

    return qosRequirement;
  }

  std::vector<Lc3Configuration> GetUnicastLc3SupportedList(bool decoding,
                                                           bool supported) {
    std::vector<Lc3Configuration> le_audio_codec_configs;
    if (!supported) {
      Lc3Configuration lc3_config{.pcmBitDepth = 0, .samplingFrequencyHz = 0};
      le_audio_codec_configs.push_back(lc3_config);
      return le_audio_codec_configs;
    }

    // There might be more than one LeAudioCodecCapabilitiesSetting
    std::vector<Lc3Capabilities> lc3_capabilities;
    for (auto& capability : temp_provider_capabilities_) {
      if (capability.getTag() != AudioCapabilities::leAudioCapabilities) {
        continue;
      }
      auto& le_audio_capability =
          capability.get<AudioCapabilities::leAudioCapabilities>();
      auto& unicast_capability =
          decoding ? le_audio_capability.unicastDecodeCapability
                   : le_audio_capability.unicastEncodeCapability;
      if (unicast_capability.codecType != CodecType::LC3) {
        continue;
      }
      auto& lc3_capability = unicast_capability.leAudioCodecCapabilities.get<
          UnicastCapability::LeAudioCodecCapabilities::lc3Capabilities>();
      lc3_capabilities.push_back(lc3_capability);
    }

    // Combine those parameters into one list of LeAudioCodecConfiguration
    // This seems horrible, but usually each Lc3Capability only contains a
    // single Lc3Configuration, which means every array has a length of 1.
    for (auto& lc3_capability : lc3_capabilities) {
      for (int32_t samplingFrequencyHz : lc3_capability.samplingFrequencyHz) {
        for (int32_t frameDurationUs : lc3_capability.frameDurationUs) {
          for (int32_t octetsPerFrame : lc3_capability.octetsPerFrame) {
            Lc3Configuration lc3_config = {
                .samplingFrequencyHz = samplingFrequencyHz,
                .frameDurationUs = frameDurationUs,
                .octetsPerFrame = octetsPerFrame,
            };
            le_audio_codec_configs.push_back(lc3_config);
          }
        }
      }
    }

    return le_audio_codec_configs;
  }

  static constexpr int32_t apx_adaptive_le_config_codec_modes[] = {0, 1, 2, 3};

  std::vector<AptxAdaptiveLeConfiguration>
  GetUnicastAptxAdaptiveLeSupportedList(bool decoding, bool supported,
                                        bool is_le_extended) {
    std::vector<AptxAdaptiveLeConfiguration> le_audio_codec_configs;
    if (!supported) {
      AptxAdaptiveLeConfiguration aptx_adaptive_le_config{
          .pcmBitDepth = 0, .samplingFrequencyHz = 0};
      le_audio_codec_configs.push_back(aptx_adaptive_le_config);
      return le_audio_codec_configs;
    }

    // There might be more than one LeAudioCodecCapabilitiesSetting
    std::vector<AptxAdaptiveLeCapabilities> aptx_adaptive_le_capabilities;
    for (auto& capability : temp_provider_capabilities_) {
      if (capability.getTag() != AudioCapabilities::leAudioCapabilities) {
        continue;
      }
      auto& le_audio_capability =
          capability.get<AudioCapabilities::leAudioCapabilities>();
      auto& unicast_capability =
          decoding ? le_audio_capability.unicastDecodeCapability
                   : le_audio_capability.unicastEncodeCapability;
      if ((!is_le_extended &&
           unicast_capability.codecType != CodecType::APTX_ADAPTIVE_LE) ||
          (is_le_extended &&
           unicast_capability.codecType != CodecType::APTX_ADAPTIVE_LEX)) {
        continue;
      }

      auto& aptx_adaptive_le_capability =
          unicast_capability.leAudioCodecCapabilities
              .get<UnicastCapability::LeAudioCodecCapabilities::
                       aptxAdaptiveLeCapabilities>();

      aptx_adaptive_le_capabilities.push_back(aptx_adaptive_le_capability);
    }

    for (auto& aptx_adaptive_le_capability : aptx_adaptive_le_capabilities) {
      for (int32_t samplingFrequencyHz :
           aptx_adaptive_le_capability.samplingFrequencyHz) {
        for (int32_t frameDurationUs :
             aptx_adaptive_le_capability.frameDurationUs) {
          for (int32_t octetsPerFrame :
               aptx_adaptive_le_capability.octetsPerFrame) {
            for (int8_t blocksPerSdu :
                 aptx_adaptive_le_capability.blocksPerSdu) {
              for (int32_t codecMode : apx_adaptive_le_config_codec_modes) {
                AptxAdaptiveLeConfiguration aptx_adaptive_le_config = {
                    .samplingFrequencyHz = samplingFrequencyHz,
                    .frameDurationUs = frameDurationUs,
                    .octetsPerFrame = octetsPerFrame,
                    .blocksPerSdu = blocksPerSdu,
                    .codecMode = codecMode,
                };
                le_audio_codec_configs.push_back(aptx_adaptive_le_config);
              }
            }
          }
        }
      }
    }

    return le_audio_codec_configs;
  }

  LeAudioCodecCapabilitiesSetting temp_le_audio_capabilities_;
  std::vector<int32_t> all_context_bitmasks = {
      AudioContext::UNSPECIFIED,   AudioContext::CONVERSATIONAL,
      AudioContext::MEDIA,         AudioContext::GAME,
      AudioContext::INSTRUCTIONAL, AudioContext::VOICE_ASSISTANTS,
      AudioContext::LIVE_AUDIO,    AudioContext::SOUND_EFFECTS,
      AudioContext::NOTIFICATIONS, AudioContext::RINGTONE_ALERTS,
      AudioContext::ALERTS,        AudioContext::EMERGENCY_ALARM,
  };

  AudioContext bidirectional_contexts = {
      .bitmask = AudioContext::CONVERSATIONAL | AudioContext::GAME |
                 AudioContext::VOICE_ASSISTANTS | AudioContext::LIVE_AUDIO,
  };
};

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be started and
 * stopped
 */
TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       OpenLeAudioOutputHardwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be started and
 * stopped with Unicast hardware encoding config taken from provider info
 */
TEST_P(
    BluetoothAudioProviderLeAudioOutputHardwareAidl,
    StartAndEndLeAudioOutputSessionWithPossibleUnicastConfigFromProviderInfo) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }
  if (!IsOffloadOutputProviderInfoSupported()) {
    GTEST_SKIP();
  }

  auto lc3_codec_configs = GetUnicastLc3SupportedListFromProviderInfo();
  LeAudioConfiguration le_audio_config = {
      .codecType = CodecType::LC3,
      .peerDelayUs = 0,
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_config.leAudioCodecConfig
        .set<LeAudioCodecConfiguration::lc3Config>(lc3_config);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_config), latency_modes,
        &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetEmptyAseConfigurationEmptyCapability) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> empty_capability;
  std::vector<LeAudioConfigurationRequirement> empty_requirement;
  std::vector<LeAudioAseConfigurationSetting> configurations;

  // Check empty capability for source direction
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, empty_capability, empty_requirement, &configurations);

  ASSERT_FALSE(aidl_retval.isOk());

  // Check empty capability for sink direction
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      empty_capability, std::nullopt, empty_requirement, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetEmptyAseConfigurationEmptyCapability_Multidirectiona) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> empty_capability;
  std::vector<LeAudioConfigurationRequirement> empty_requirement;
  std::vector<LeAudioAseConfigurationSetting> configurations;

  // Check empty capability for source direction
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, empty_capability, empty_requirement, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());

  // Check empty capability for sink direction
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      empty_capability, std::nullopt, empty_requirement, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetEmptyAseConfigurationMismatchedRequirement) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }
  std::vector<std::optional<LeAudioDeviceCapabilities>> sink_capabilities = {
      GetDefaultRemoteSinkCapability()};
  std::vector<std::optional<LeAudioDeviceCapabilities>> source_capabilities = {
      GetDefaultRemoteSourceCapability()};

  auto not_supported_sampling_rate_by_remote =
      CodecSpecificConfigurationLtv::SamplingFrequency::HZ11025;

  // Check empty capability for source direction
  std::vector<LeAudioAseConfigurationSetting> configurations;
  std::vector<LeAudioConfigurationRequirement> source_requirements = {
      GetUnicastDefaultRequirement(AudioContext::LIVE_AUDIO, false /*sink */,
                                   true /* source */,
                                   not_supported_sampling_rate_by_remote)};
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, source_capabilities, source_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());

  // Check empty capability for sink direction
  std::vector<LeAudioConfigurationRequirement> sink_requirements = {
      GetUnicastDefaultRequirement(AudioContext::MEDIA, true /*sink */,
                                   false /* source */,
                                   not_supported_sampling_rate_by_remote)};
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, std::nullopt, sink_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl, GetAseConfiguration) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> sink_capabilities = {
      GetDefaultRemoteSinkCapability()};
  std::vector<std::optional<LeAudioDeviceCapabilities>> source_capabilities = {
      GetDefaultRemoteSourceCapability()};

  // Should not ask for Source on ENCODING session if Multidiretional not
  // supported
  std::vector<LeAudioAseConfigurationSetting> configurations;
  std::vector<LeAudioConfigurationRequirement> source_requirements = {
      GetUnicastDefaultRequirement(AudioContext::LIVE_AUDIO, false /* sink */,
                                   true /* source */)};
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, source_capabilities, source_requirements, &configurations);

  ASSERT_FALSE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());

  // Check capability for remote sink direction
  std::vector<LeAudioConfigurationRequirement> sink_requirements = {
      GetUnicastDefaultRequirement(AudioContext::MEDIA, true /* sink */,
                                   false /* source */)};
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, std::nullopt, sink_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(sink_requirements, configurations);

  // Check multiple capability for remote sink direction
  std::vector<LeAudioConfigurationRequirement> multi_sink_requirements = {
      GetUnicastDefaultRequirement(AudioContext::MEDIA, true /* sink */,
                                   false /* source */),
      GetUnicastDefaultRequirement(AudioContext::CONVERSATIONAL,
                                   true /* sink */, false /* source */)};
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, std::nullopt, multi_sink_requirements,
      &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(multi_sink_requirements, configurations);

  // Check multiple context types in a single requirement.
  std::vector<LeAudioConfigurationRequirement> multi_context_sink_requirements =
      {GetUnicastDefaultRequirement(
          AudioContext::MEDIA | AudioContext::SOUND_EFFECTS, true /* sink */,
          false /* source */)};
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, std::nullopt, multi_context_sink_requirements,
      &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(multi_sink_requirements, configurations);
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetOpusAseConfiguration) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> sink_capabilities = {
      GetOpusRemoteSinkCapability()};
  std::vector<std::optional<LeAudioDeviceCapabilities>> source_capabilities = {
      GetDefaultRemoteSourceCapability()};

  std::vector<LeAudioAseConfigurationSetting> configurations;
  std::vector<LeAudioConfigurationRequirement> sink_requirements = {
      GetOpusUnicastRequirement(AudioContext::MEDIA, true /* sink */,
                                false /* source */)};
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, std::nullopt, sink_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  if (!configurations.empty()) {
    VerifyIfRequirementsSatisfied(sink_requirements, configurations);
  }
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetAseConfiguration_Multidirectional) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> sink_capabilities = {
      GetDefaultRemoteSinkCapability()};
  std::vector<std::optional<LeAudioDeviceCapabilities>> source_capabilities = {
      GetDefaultRemoteSourceCapability()};

  // Verify source configuration is received
  std::vector<LeAudioAseConfigurationSetting> configurations;
  std::vector<LeAudioConfigurationRequirement> source_requirements = {
      GetUnicastDefaultRequirement(AudioContext::LIVE_AUDIO, false /* sink */,
                                   true /* source */)};
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, source_capabilities, source_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(source_requirements, configurations);

  // Verify sink configuration is received
  std::vector<LeAudioConfigurationRequirement> sink_requirements = {
      GetUnicastDefaultRequirement(AudioContext::MEDIA, true /* sink */,
                                   false /* source */)};
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, std::nullopt, sink_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(sink_requirements, configurations);

  std::vector<LeAudioConfigurationRequirement> combined_requirements = {
      GetUnicastDefaultRequirement(AudioContext::LIVE_AUDIO, false /* sink */,
                                   true /* source */),
      GetUnicastDefaultRequirement(AudioContext::CONVERSATIONAL,
                                   true /* sink */, true /* source */),
      GetUnicastDefaultRequirement(AudioContext::MEDIA, true /* sink */,
                                   false /* source */)};

  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, source_capabilities, combined_requirements,
      &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(combined_requirements, configurations);
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetAsymmetricAseConfiguration_Multidirectional) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  if (!IsAsymmetricConfigurationAllowed()) {
    GTEST_SKIP();
  }

  std::vector<LeAudioAseConfigurationSetting> configurations;
  std::vector<std::optional<LeAudioDeviceCapabilities>> sink_capabilities = {
      GetDefaultRemoteSinkCapability()};
  std::vector<std::optional<LeAudioDeviceCapabilities>> source_capabilities = {
      GetDefaultRemoteSourceCapability()};

  std::vector<LeAudioConfigurationRequirement> asymmetric_requirements = {
      GetUnicastGameRequirement(true /* Asymmetric */)};

  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, source_capabilities, asymmetric_requirements,
      &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(asymmetric_requirements, configurations);
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetQoSConfiguration_Multidirectional) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
  allocation.bitmask =
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

  LeAudioAseQosConfigurationRequirement requirement =
      GetQosRequirements(true, true);

  std::vector<IBluetoothAudioProvider::LeAudioAseQosConfiguration>
      QoSConfigurations;
  bool is_supported = false;
  for (auto bitmask : all_context_bitmasks) {
    requirement.audioContext = GetAudioContext(bitmask);
    bool is_bidirectional = bidirectional_contexts.bitmask & bitmask;

    if (is_bidirectional) {
      requirement.sourceAseQosRequirement = requirement.sinkAseQosRequirement;
    } else {
      requirement.sourceAseQosRequirement = std::nullopt;
    }

    IBluetoothAudioProvider::LeAudioAseQosConfigurationPair result;
    auto aidl_retval =
        audio_provider_->getLeAudioAseQosConfiguration(requirement, &result);
    if (!aidl_retval.isOk()) {
      // If not OK, then it could be not supported, as it is an optional
      // feature
      ASSERT_EQ(aidl_retval.getExceptionCode(), EX_UNSUPPORTED_OPERATION);
    }

    is_supported = true;
    if (result.sinkQosConfiguration.has_value()) {
      if (is_bidirectional) {
        ASSERT_TRUE(result.sourceQosConfiguration.has_value());
      } else {
        ASSERT_FALSE(result.sourceQosConfiguration.has_value());
      }
      QoSConfigurations.push_back(result.sinkQosConfiguration.value());
    }
  }
  if (is_supported) {
    // QoS Configurations should not be empty, as we searched for all contexts
    ASSERT_FALSE(QoSConfigurations.empty());
  }
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetQoSConfiguration_InvalidRequirements) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }
  auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
  allocation.bitmask =
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

  LeAudioAseQosConfigurationRequirement invalid_requirement =
      GetQosRequirements(true /* sink */, false /* source */,
                         false /* valid */);

  std::vector<IBluetoothAudioProvider::LeAudioAseQosConfiguration>
      QoSConfigurations;
  for (auto bitmask : all_context_bitmasks) {
    invalid_requirement.audioContext = GetAudioContext(bitmask);
    IBluetoothAudioProvider::LeAudioAseQosConfigurationPair result;
    auto aidl_retval = audio_provider_->getLeAudioAseQosConfiguration(
        invalid_requirement, &result);
    ASSERT_FALSE(aidl_retval.isOk());
  }
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl, GetQoSConfiguration) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }
  auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
  allocation.bitmask =
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

  IBluetoothAudioProvider::LeAudioAseQosConfigurationRequirement requirement;
  requirement = GetQosRequirements(true /* sink */, false /* source */);

  std::vector<IBluetoothAudioProvider::LeAudioAseQosConfiguration>
      QoSConfigurations;
  bool is_supported = false;
  for (auto bitmask : all_context_bitmasks) {
    requirement.audioContext = GetAudioContext(bitmask);
    IBluetoothAudioProvider::LeAudioAseQosConfigurationPair result;
    auto aidl_retval =
        audio_provider_->getLeAudioAseQosConfiguration(requirement, &result);
    if (!aidl_retval.isOk()) {
      // If not OK, then it could be not supported, as it is an optional
      // feature
      ASSERT_EQ(aidl_retval.getExceptionCode(), EX_UNSUPPORTED_OPERATION);
    } else {
      is_supported = true;
      if (result.sinkQosConfiguration.has_value()) {
        QoSConfigurations.push_back(result.sinkQosConfiguration.value());
      }
    }
  }

  if (is_supported) {
    // QoS Configurations should not be empty, as we searched for all contexts
    ASSERT_FALSE(QoSConfigurations.empty());
  }
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetDataPathConfiguration_Multidirectional) {
  IBluetoothAudioProvider::StreamConfig sink_requirement;
  IBluetoothAudioProvider::StreamConfig source_requirement;
  std::vector<IBluetoothAudioProvider::LeAudioDataPathConfiguration>
      DataPathConfigurations;

  if (!IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  bool is_supported = false;
  auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
  allocation.bitmask =
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

  auto streamMap = LeAudioConfiguration::StreamMap();

  // Use some mandatory configuration
  streamMap.streamHandle = 0x0001;
  streamMap.audioChannelAllocation = 0x03;
  streamMap.aseConfiguration = {
      .targetLatency =
          LeAudioAseConfiguration::TargetLatency::BALANCED_LATENCY_RELIABILITY,
      .codecId = CodecId::Core::LC3,
      .codecConfiguration =
          {CodecSpecificConfigurationLtv::SamplingFrequency::HZ16000,
           CodecSpecificConfigurationLtv::FrameDuration::US10000, allocation},
  };

  // Bidirectional
  sink_requirement.streamMap = {streamMap};
  source_requirement.streamMap = {streamMap};

  for (auto bitmask : all_context_bitmasks) {
    sink_requirement.audioContext = GetAudioContext(bitmask);
    source_requirement.audioContext = sink_requirement.audioContext;

    IBluetoothAudioProvider::LeAudioDataPathConfigurationPair result;
    ::ndk::ScopedAStatus aidl_retval;

    bool is_bidirectional = bidirectional_contexts.bitmask & bitmask;
    if (is_bidirectional) {
      aidl_retval = audio_provider_->getLeAudioAseDatapathConfiguration(
          sink_requirement, source_requirement, &result);
    } else {
      aidl_retval = audio_provider_->getLeAudioAseDatapathConfiguration(
          sink_requirement, std::nullopt, &result);
    }

    if (!aidl_retval.isOk()) {
      // If not OK, then it could be not supported, as it is an optional
      // feature
      ASSERT_EQ(aidl_retval.getExceptionCode(), EX_UNSUPPORTED_OPERATION);
    } else {
      is_supported = true;
      if (result.outputConfig.has_value()) {
        if (is_bidirectional) {
          ASSERT_TRUE(result.inputConfig.has_value());
        } else {
          ASSERT_TRUE(!result.inputConfig.has_value());
        }
        DataPathConfigurations.push_back(result.outputConfig.value());
      }
    }
  }

  if (is_supported) {
    // Datapath Configurations should not be empty, as we searched for all
    // contexts
    ASSERT_FALSE(DataPathConfigurations.empty());
  }
}

TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       GetDataPathConfiguration) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }
  IBluetoothAudioProvider::StreamConfig sink_requirement;
  std::vector<IBluetoothAudioProvider::LeAudioDataPathConfiguration>
      DataPathConfigurations;
  bool is_supported = false;
  auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
  allocation.bitmask =
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

  auto streamMap = LeAudioConfiguration::StreamMap();

  // Use some mandatory configuration
  streamMap.streamHandle = 0x0001;
  streamMap.audioChannelAllocation = 0x03;
  streamMap.aseConfiguration = {
      .targetLatency =
          LeAudioAseConfiguration::TargetLatency::BALANCED_LATENCY_RELIABILITY,
      .codecId = CodecId::Core::LC3,
      .codecConfiguration =
          {CodecSpecificConfigurationLtv::SamplingFrequency::HZ16000,
           CodecSpecificConfigurationLtv::FrameDuration::US10000, allocation},
  };

  sink_requirement.streamMap = {streamMap};

  for (auto bitmask : all_context_bitmasks) {
    sink_requirement.audioContext = GetAudioContext(bitmask);
    IBluetoothAudioProvider::LeAudioDataPathConfigurationPair result;
    auto aidl_retval = audio_provider_->getLeAudioAseDatapathConfiguration(
        sink_requirement, std::nullopt, &result);

    if (!aidl_retval.isOk()) {
      // If not OK, then it could be not supported, as it is an optional
      // feature
      ASSERT_EQ(aidl_retval.getExceptionCode(), EX_UNSUPPORTED_OPERATION);
    } else {
      is_supported = true;
      if (result.outputConfig.has_value()) {
        DataPathConfigurations.push_back(result.outputConfig.value());
      }
    }
  }

  if (is_supported) {
    // Datapath Configurations should not be empty, as we searched for all
    // contexts
    ASSERT_FALSE(DataPathConfigurations.empty());
  }
}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be started and
 * stopped with Unicast hardware encoding config
 */
TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       StartAndEndLeAudioOutputSessionWithPossibleUnicastConfig) {
  if (!IsOffloadOutputSupported()) {
    GTEST_SKIP();
  }

  auto lc3_codec_configs =
      GetUnicastLc3SupportedList(false /* decoding */, true /* supported */);
  LeAudioConfiguration le_audio_config = {
      .codecType = CodecType::LC3,
      .peerDelayUs = 0,
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_config.leAudioCodecConfig
        .set<LeAudioCodecConfiguration::lc3Config>(lc3_config);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_config), latency_modes,
        &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be started and
 * stopped with Unicast hardware encoding config
 *
 */
TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       StartAndEndLeAudioOutputSessionWithInvalidAudioConfiguration) {
  if (!IsOffloadOutputSupported()) {
    GTEST_SKIP();
  }

  auto lc3_codec_configs =
      GetUnicastLc3SupportedList(false /* decoding */, false /* supported */);
  LeAudioConfiguration le_audio_config = {
      .codecType = CodecType::LC3,
      .peerDelayUs = 0,
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_config.leAudioCodecConfig
        .set<LeAudioCodecConfiguration::lc3Config>(lc3_config);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_config), latency_modes,
        &mq_desc);

    // It is OK to start session with invalid configuration
    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

static std::vector<uint8_t> vendorMetadata = {0x0B,  // Length
                                              0xFF,  // Type: Vendor-specific
                                              0x0A, 0x00,  // Company_ID
                                              0x01, 0x02, 0x03, 0x04,  // Data
                                              0x05, 0x06, 0x07, 0x08};

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be started and
 * stopped with Unicast hardware encoding config
 */
TEST_P(BluetoothAudioProviderLeAudioOutputHardwareAidl,
       StartAndEndLeAudioOutputSessionWithAptxAdaptiveLeUnicastConfig) {
  if (!IsOffloadOutputSupported()) {
    GTEST_SKIP();
  }
  for (auto codec_type :
       {CodecType::APTX_ADAPTIVE_LE, CodecType::APTX_ADAPTIVE_LEX}) {
    bool is_le_extended = (codec_type == CodecType::APTX_ADAPTIVE_LEX);
    auto aptx_adaptive_le_codec_configs =
        GetUnicastAptxAdaptiveLeSupportedList(false, true, is_le_extended);
    LeAudioConfiguration le_audio_config = {
        .codecType = codec_type,
        .peerDelayUs = 0,
        .vendorSpecificMetadata = vendorMetadata,
    };

    for (auto& aptx_adaptive_le_config : aptx_adaptive_le_codec_configs) {
      le_audio_config.leAudioCodecConfig
          .set<LeAudioCodecConfiguration::aptxAdaptiveLeConfig>(
              aptx_adaptive_le_config);
      DataMQDesc mq_desc;
      auto aidl_retval = audio_provider_->startSession(
          audio_port_, AudioConfiguration(le_audio_config), latency_modes,
          &mq_desc);

      ASSERT_TRUE(aidl_retval.isOk());
      EXPECT_TRUE(audio_provider_->endSession().isOk());
    }
  }
}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be started and
 * stopped with Unicast hardware encoding config
 */
TEST_P(
    BluetoothAudioProviderLeAudioOutputHardwareAidl,
    BluetoothAudioProviderLeAudioOutputHardwareAidl_StartAndEndLeAudioOutputSessionWithInvalidAptxAdaptiveLeAudioConfiguration) {
  if (!IsOffloadOutputSupported()) {
    GTEST_SKIP();
  }

  for (auto codec_type :
       {CodecType::APTX_ADAPTIVE_LE, CodecType::APTX_ADAPTIVE_LEX}) {
    bool is_le_extended = (codec_type == CodecType::APTX_ADAPTIVE_LEX);
    auto aptx_adaptive_le_codec_configs =
        GetUnicastAptxAdaptiveLeSupportedList(false, true, is_le_extended);
    LeAudioConfiguration le_audio_config = {
        .codecType = codec_type,
        .peerDelayUs = 0,
        .vendorSpecificMetadata = vendorMetadata,
    };

    for (auto& aptx_adaptive_le_config : aptx_adaptive_le_codec_configs) {
      le_audio_config.leAudioCodecConfig
          .set<LeAudioCodecConfiguration::aptxAdaptiveLeConfig>(
              aptx_adaptive_le_config);
      DataMQDesc mq_desc;
      auto aidl_retval = audio_provider_->startSession(
          audio_port_, AudioConfiguration(le_audio_config), latency_modes,
          &mq_desc);

      // It is OK to start session with invalid configuration
      ASSERT_TRUE(aidl_retval.isOk());
      EXPECT_TRUE(audio_provider_->endSession().isOk());
    }
  }
}

/**
 * openProvider LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH
 */
class BluetoothAudioProviderLeAudioInputHardwareAidl
    : public BluetoothAudioProviderLeAudioOutputHardwareAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH);
    GetProviderInfoHelper(
        SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH);
    OpenProviderHelper(
        SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH);
    ASSERT_TRUE(temp_provider_capabilities_.empty() ||
                audio_provider_ != nullptr);
  }

  bool IsOffloadInputSupported() {
    for (auto& capability : temp_provider_capabilities_) {
      if (capability.getTag() != AudioCapabilities::leAudioCapabilities) {
        continue;
      }
      auto& le_audio_capability =
          capability.get<AudioCapabilities::leAudioCapabilities>();
      if (le_audio_capability.unicastDecodeCapability.codecType !=
          CodecType::UNKNOWN)
        return true;
    }
    return false;
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }
};

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH can be started and
 * stopped
 */
TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl,
       OpenLeAudioInputHardwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH can be started and
 * stopped with Unicast hardware encoding config taken from provider info
 */
TEST_P(
    BluetoothAudioProviderLeAudioInputHardwareAidl,
    StartAndEndLeAudioInputSessionWithPossibleUnicastConfigFromProviderInfo) {
  if (!IsOffloadOutputProviderInfoSupported()) {
    GTEST_SKIP();
  }

  auto lc3_codec_configs = GetUnicastLc3SupportedListFromProviderInfo();
  LeAudioConfiguration le_audio_config = {
      .codecType = CodecType::LC3,
      .peerDelayUs = 0,
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_config.leAudioCodecConfig
        .set<LeAudioCodecConfiguration::lc3Config>(lc3_config);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_config), latency_modes,
        &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH can be started and
 * stopped with Unicast hardware encoding config
 */
TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl,
       StartAndEndLeAudioInputSessionWithPossibleUnicastConfig) {
  if (!IsOffloadInputSupported()) {
    GTEST_SKIP();
  }

  auto lc3_codec_configs =
      GetUnicastLc3SupportedList(true /* decoding */, true /* supported */);
  LeAudioConfiguration le_audio_config = {
      .codecType = CodecType::LC3,
      .peerDelayUs = 0,
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_config.leAudioCodecConfig
        .set<LeAudioCodecConfiguration::lc3Config>(lc3_config);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_config), latency_modes,
        &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH can be started and
 * stopped with Unicast hardware encoding config
 *
 */
TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl,
       StartAndEndLeAudioInputSessionWithInvalidAudioConfiguration) {
  if (!IsOffloadInputSupported()) {
    GTEST_SKIP();
  }

  auto lc3_codec_configs =
      GetUnicastLc3SupportedList(true /* decoding */, false /* supported */);
  LeAudioConfiguration le_audio_config = {
      .codecType = CodecType::LC3,
      .peerDelayUs = 0,
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_config.leAudioCodecConfig
        .set<LeAudioCodecConfiguration::lc3Config>(lc3_config);

    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_config), latency_modes,
        &mq_desc);

    // It is OK to start with invalid configuration as it might be unknown on
    // start
    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl,
       GetEmptyAseConfigurationEmptyCapability) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> empty_capability;
  std::vector<LeAudioConfigurationRequirement> empty_requirement;
  std::vector<LeAudioAseConfigurationSetting> configurations;

  // Check success for source direction (Input == DecodingSession == remote
  // source)
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, empty_capability, empty_requirement, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());

  // Check failure for sink direction
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      empty_capability, std::nullopt, empty_requirement, &configurations);

  ASSERT_FALSE(aidl_retval.isOk());
}

TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl,
       GetEmptyAseConfigurationEmptyCapability_Multidirectional) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> empty_capability;
  std::vector<LeAudioConfigurationRequirement> empty_requirement;
  std::vector<LeAudioAseConfigurationSetting> configurations;

  // Check empty capability for source direction
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, empty_capability, empty_requirement, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());

  // Check empty capability for sink direction
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      empty_capability, std::nullopt, empty_requirement, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_TRUE(configurations.empty());
}

TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl, GetAseConfiguration) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> sink_capabilities = {
      GetDefaultRemoteSinkCapability()};
  std::vector<std::optional<LeAudioDeviceCapabilities>> source_capabilities = {
      GetDefaultRemoteSourceCapability()};

  // Check source configuration is received
  std::vector<LeAudioAseConfigurationSetting> configurations;
  std::vector<LeAudioConfigurationRequirement> source_requirements = {
      GetUnicastDefaultRequirement(AudioContext::LIVE_AUDIO, false /* sink */,
                                   true /* source */)};
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, source_capabilities, source_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());

  // Check error result when requesting sink on DECODING session
  std::vector<LeAudioConfigurationRequirement> sink_requirements = {
      GetUnicastDefaultRequirement(AudioContext::MEDIA, true /* sink */,
                                   false /* source */)};
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, std::nullopt, sink_requirements, &configurations);

  ASSERT_FALSE(aidl_retval.isOk());
}

TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl,
       GetAseConfiguration_Multidirectional) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsMultidirectionalCapabilitiesEnabled()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> sink_capabilities = {
      GetDefaultRemoteSinkCapability()};
  std::vector<std::optional<LeAudioDeviceCapabilities>> source_capabilities = {
      GetDefaultRemoteSourceCapability()};

  // Check source configuration is received
  std::vector<LeAudioAseConfigurationSetting> configurations;
  std::vector<LeAudioConfigurationRequirement> source_requirements = {
      GetUnicastDefaultRequirement(AudioContext::LIVE_AUDIO, false /* sink */,
                                   true /* source */)};
  auto aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      std::nullopt, source_capabilities, source_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(source_requirements, configurations);

  // Check empty capability for sink direction
  std::vector<LeAudioConfigurationRequirement> sink_requirements = {
      GetUnicastDefaultRequirement(AudioContext::MEDIA, true /* sink */,
                                   false /* source */)};
  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, std::nullopt, sink_requirements, &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(sink_requirements, configurations);

  std::vector<LeAudioConfigurationRequirement> combined_requirements = {
      GetUnicastDefaultRequirement(AudioContext::LIVE_AUDIO, false /* sink */,
                                   true /* source */),
      GetUnicastDefaultRequirement(AudioContext::CONVERSATIONAL,
                                   true /* sink */, true /* source */),
      GetUnicastDefaultRequirement(AudioContext::MEDIA, true /* sink */,
                                   false /* source */)};

  aidl_retval = audio_provider_->getLeAudioAseConfiguration(
      sink_capabilities, source_capabilities, combined_requirements,
      &configurations);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_FALSE(configurations.empty());
  VerifyIfRequirementsSatisfied(combined_requirements, configurations);
}

TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl,
       GetQoSConfiguration_InvalidRequirements) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }
  auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
  allocation.bitmask =
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

  LeAudioAseQosConfigurationRequirement invalid_requirement =
      GetQosRequirements(false /* sink */, true /* source */,
                         false /* valid */);

  std::vector<IBluetoothAudioProvider::LeAudioAseQosConfiguration>
      QoSConfigurations;
  for (auto bitmask : all_context_bitmasks) {
    invalid_requirement.audioContext = GetAudioContext(bitmask);
    IBluetoothAudioProvider::LeAudioAseQosConfigurationPair result;
    auto aidl_retval = audio_provider_->getLeAudioAseQosConfiguration(
        invalid_requirement, &result);
    ASSERT_FALSE(aidl_retval.isOk());
  }
}

TEST_P(BluetoothAudioProviderLeAudioInputHardwareAidl, GetQoSConfiguration) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }
  auto allocation = CodecSpecificConfigurationLtv::AudioChannelAllocation();
  allocation.bitmask =
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_LEFT |
      CodecSpecificConfigurationLtv::AudioChannelAllocation::FRONT_RIGHT;

  IBluetoothAudioProvider::LeAudioAseQosConfigurationRequirement requirement;
  requirement = GetQosRequirements(false /* sink */, true /* source */);

  std::vector<IBluetoothAudioProvider::LeAudioAseQosConfiguration>
      QoSConfigurations;
  bool is_supported = false;
  for (auto bitmask : all_context_bitmasks) {
    requirement.audioContext = GetAudioContext(bitmask);
    IBluetoothAudioProvider::LeAudioAseQosConfigurationPair result;
    auto aidl_retval =
        audio_provider_->getLeAudioAseQosConfiguration(requirement, &result);
    if (!aidl_retval.isOk()) {
      // If not OK, then it could be not supported, as it is an optional
      // feature
      ASSERT_EQ(aidl_retval.getExceptionCode(), EX_UNSUPPORTED_OPERATION);
    } else {
      is_supported = true;
      if (result.sourceQosConfiguration.has_value()) {
        QoSConfigurations.push_back(result.sourceQosConfiguration.value());
      }
    }
  }

  if (is_supported) {
    // QoS Configurations should not be empty, as we searched for all contexts
    ASSERT_FALSE(QoSConfigurations.empty());
  }
}
/**
 * openProvider LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH
 */
class BluetoothAudioProviderLeAudioBroadcastSoftwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH);
    OpenProviderHelper(
        SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH);
    ASSERT_NE(audio_provider_, nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  static constexpr int32_t le_audio_output_sample_rates_[] = {
      0, 8000, 16000, 24000, 32000, 44100, 48000,
  };
  static constexpr int8_t le_audio_output_bits_per_samples_[] = {0, 16, 24};
  static constexpr ChannelMode le_audio_output_channel_modes_[] = {
      ChannelMode::UNKNOWN, ChannelMode::MONO, ChannelMode::STEREO};
  static constexpr int32_t le_audio_output_data_interval_us_[] = {
      0 /* Invalid */, 10000 /* Valid 10ms */};
};

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH can be started
 * and stopped
 */
TEST_P(BluetoothAudioProviderLeAudioBroadcastSoftwareAidl,
       OpenLeAudioOutputSoftwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH can be started
 * and stopped with different PCM config
 */
TEST_P(BluetoothAudioProviderLeAudioBroadcastSoftwareAidl,
       StartAndEndLeAudioOutputSessionWithPossiblePcmConfig) {
  for (auto sample_rate : le_audio_output_sample_rates_) {
    for (auto bits_per_sample : le_audio_output_bits_per_samples_) {
      for (auto channel_mode : le_audio_output_channel_modes_) {
        for (auto data_interval_us : le_audio_output_data_interval_us_) {
          PcmConfiguration pcm_config{
              .sampleRateHz = sample_rate,
              .channelMode = channel_mode,
              .bitsPerSample = bits_per_sample,
              .dataIntervalUs = data_interval_us,
          };
          bool is_codec_config_valid =
              IsPcmConfigSupported(pcm_config) && pcm_config.dataIntervalUs > 0;
          DataMQDesc mq_desc;
          auto aidl_retval = audio_provider_->startSession(
              audio_port_, AudioConfiguration(pcm_config), latency_modes,
              &mq_desc);
          DataMQ data_mq(mq_desc);

          EXPECT_EQ(aidl_retval.isOk(), is_codec_config_valid);
          if (is_codec_config_valid) {
            EXPECT_TRUE(data_mq.isValid());
          }
          EXPECT_TRUE(audio_provider_->endSession().isOk());
        }
      }
    }
  }
}

/**
 * openProvider LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH
 */
class BluetoothAudioProviderLeAudioBroadcastHardwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
    GetProviderInfoHelper(
        SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
    OpenProviderHelper(
        SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
    ASSERT_TRUE(temp_provider_capabilities_.empty() ||
                audio_provider_ != nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  bool IsBroadcastOffloadSupported() {
    for (auto& capability : temp_provider_capabilities_) {
      if (capability.getTag() != AudioCapabilities::leAudioCapabilities) {
        continue;
      }
      auto& le_audio_capability =
          capability.get<AudioCapabilities::leAudioCapabilities>();
      if (le_audio_capability.broadcastCapability.codecType !=
          CodecType::UNKNOWN)
        return true;
    }
    return false;
  }

  bool IsBroadcastOffloadProviderInfoSupported() {
    if (!temp_provider_info_.has_value()) return false;
    if (temp_provider_info_.value().codecInfos.empty()) return false;
    // Check if all codec info is of LeAudio type
    for (auto& codec_info : temp_provider_info_.value().codecInfos) {
      if (codec_info.transport.getTag() != CodecInfo::Transport::leAudio)
        return false;
    }
    return true;
  }

  std::vector<Lc3Configuration> GetBroadcastLc3SupportedListFromProviderInfo() {
    std::vector<Lc3Configuration> le_audio_codec_configs;
    for (auto& codec_info : temp_provider_info_.value().codecInfos) {
      // Only gets LC3 codec information
      if (codec_info.id != CodecId::Core::LC3) continue;
      // Combine those parameters into one list of Lc3Configuration
      auto& transport =
          codec_info.transport.get<CodecInfo::Transport::Tag::leAudio>();
      for (int32_t samplingFrequencyHz : transport.samplingFrequencyHz) {
        for (int32_t frameDurationUs : transport.frameDurationUs) {
          for (int32_t octetsPerFrame : transport.bitdepth) {
            Lc3Configuration lc3_config = {
                .samplingFrequencyHz = samplingFrequencyHz,
                .frameDurationUs = frameDurationUs,
                .octetsPerFrame = octetsPerFrame,
            };
            le_audio_codec_configs.push_back(lc3_config);
          }
        }
      }
    }

    return le_audio_codec_configs;
  }

  AudioContext GetAudioContext(int32_t bitmask) {
    AudioContext media_audio_context;
    media_audio_context.bitmask = bitmask;
    return media_audio_context;
  }

  std::optional<CodecSpecificConfigurationLtv::SamplingFrequency>
  GetBisSampleFreq(const LeAudioBisConfiguration& bis_conf) {
    auto sample_freq_ltv = GetConfigurationLtv(
        bis_conf.codecConfiguration,
        CodecSpecificConfigurationLtv::Tag::samplingFrequency);
    if (!sample_freq_ltv) {
      return std::nullopt;
    }
    return (*sample_freq_ltv)
        .get<CodecSpecificConfigurationLtv::samplingFrequency>();
  }

  std::vector<CodecSpecificConfigurationLtv::SamplingFrequency>
  GetSubgroupSampleFreqs(
      const LeAudioBroadcastSubgroupConfiguration& subgroup_conf) {
    std::vector<CodecSpecificConfigurationLtv::SamplingFrequency> result = {};

    for (const auto& bis_conf : subgroup_conf.bisConfigurations) {
      auto sample_freq = GetBisSampleFreq(bis_conf.bisConfiguration);
      if (sample_freq) {
        result.push_back(*sample_freq);
      }
    }
    return result;
  }

  void VerifyBroadcastConfiguration(
      const LeAudioBroadcastConfigurationRequirement& requirements,
      const LeAudioBroadcastConfigurationSetting& configuration,
      std::vector<CodecSpecificConfigurationLtv::SamplingFrequency>
          expectedSampleFreqs = {}) {
    std::vector<CodecSpecificConfigurationLtv::SamplingFrequency> sampleFreqs =
        {};

    int number_of_requested_bises = 0;
    for (const auto& subgroup_req :
         requirements.subgroupConfigurationRequirements) {
      number_of_requested_bises += subgroup_req.bisNumPerSubgroup;
    }

    if (!expectedSampleFreqs.empty()) {
      for (const auto& subgroup_conf : configuration.subgroupsConfigurations) {
        auto result = GetSubgroupSampleFreqs(subgroup_conf);
        sampleFreqs.insert(sampleFreqs.end(), result.begin(), result.end());
      }
    }

    ASSERT_EQ(number_of_requested_bises, configuration.numBis);
    ASSERT_EQ(requirements.subgroupConfigurationRequirements.size(),
              configuration.subgroupsConfigurations.size());

    if (expectedSampleFreqs.empty()) {
      return;
    }

    std::sort(sampleFreqs.begin(), sampleFreqs.end());
    std::sort(expectedSampleFreqs.begin(), expectedSampleFreqs.end());

    ASSERT_EQ(sampleFreqs, expectedSampleFreqs);
  }

  LeAudioDeviceCapabilities GetDefaultBroadcastSinkCapability() {
    // Create a capability
    LeAudioDeviceCapabilities capability;

    capability.codecId = CodecId::Core::LC3;

    auto pref_context_metadata = MetadataLtv::PreferredAudioContexts();
    pref_context_metadata.values =
        GetAudioContext(AudioContext::MEDIA | AudioContext::CONVERSATIONAL |
                        AudioContext::GAME);
    capability.metadata = {pref_context_metadata};

    auto sampling_rate =
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies();
    sampling_rate.bitmask =
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ48000 |
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ24000 |
        CodecSpecificCapabilitiesLtv::SupportedSamplingFrequencies::HZ16000;
    auto frame_duration =
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations();
    frame_duration.bitmask =
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US7500 |
        CodecSpecificCapabilitiesLtv::SupportedFrameDurations::US10000;
    auto octets = CodecSpecificCapabilitiesLtv::SupportedOctetsPerCodecFrame();
    octets.min = 0;
    octets.max = 120;
    auto frames = CodecSpecificCapabilitiesLtv::SupportedMaxCodecFramesPerSDU();
    frames.value = 2;
    capability.codecSpecificCapabilities = {sampling_rate, frame_duration,
                                            octets, frames};
    return capability;
  }

  LeAudioBroadcastConfigurationRequirement GetBroadcastRequirement(
      bool standard_quality, bool high_quality) {
    LeAudioBroadcastConfigurationRequirement requirement;

    AudioContext media_audio_context;
    media_audio_context.bitmask = AudioContext::MEDIA;

    LeAudioBroadcastSubgroupConfigurationRequirement
        standard_quality_requirement = {
            .audioContext = media_audio_context,
            .quality = IBluetoothAudioProvider::BroadcastQuality::STANDARD,
            .bisNumPerSubgroup = 2};

    LeAudioBroadcastSubgroupConfigurationRequirement high_quality_requirement =
        {.audioContext = media_audio_context,
         .quality = IBluetoothAudioProvider::BroadcastQuality::HIGH,
         .bisNumPerSubgroup = 2};

    if (standard_quality) {
      requirement.subgroupConfigurationRequirements.push_back(
          standard_quality_requirement);
    }

    if (high_quality) {
      requirement.subgroupConfigurationRequirements.push_back(
          high_quality_requirement);
    }
    return requirement;
  }

  std::vector<Lc3Configuration> GetBroadcastLc3SupportedList(bool supported) {
    std::vector<Lc3Configuration> le_audio_codec_configs;
    if (!supported) {
      Lc3Configuration lc3_config{.pcmBitDepth = 0, .samplingFrequencyHz = 0};
      le_audio_codec_configs.push_back(lc3_config);
      return le_audio_codec_configs;
    }

    // There might be more than one LeAudioCodecCapabilitiesSetting
    std::vector<Lc3Capabilities> lc3_capabilities;
    for (auto& capability : temp_provider_capabilities_) {
      if (capability.getTag() != AudioCapabilities::leAudioCapabilities) {
        continue;
      }
      auto& le_audio_capability =
          capability.get<AudioCapabilities::leAudioCapabilities>();
      auto& broadcast_capability = le_audio_capability.broadcastCapability;
      if (broadcast_capability.codecType != CodecType::LC3) {
        continue;
      }
      auto& lc3_capability = broadcast_capability.leAudioCodecCapabilities.get<
          BroadcastCapability::LeAudioCodecCapabilities::lc3Capabilities>();
      for (int idx = 0; idx < lc3_capability->size(); idx++)
        lc3_capabilities.push_back(*lc3_capability->at(idx));
    }

    // Combine those parameters into one list of LeAudioCodecConfiguration
    // This seems horrible, but usually each Lc3Capability only contains a
    // single Lc3Configuration, which means every array has a length of 1.
    for (auto& lc3_capability : lc3_capabilities) {
      for (int32_t samplingFrequencyHz : lc3_capability.samplingFrequencyHz) {
        for (int32_t frameDurationUs : lc3_capability.frameDurationUs) {
          for (int32_t octetsPerFrame : lc3_capability.octetsPerFrame) {
            Lc3Configuration lc3_config = {
                .samplingFrequencyHz = samplingFrequencyHz,
                .frameDurationUs = frameDurationUs,
                .octetsPerFrame = octetsPerFrame,
            };
            le_audio_codec_configs.push_back(lc3_config);
          }
        }
      }
    }

    return le_audio_codec_configs;
  }

  LeAudioCodecCapabilitiesSetting temp_le_audio_capabilities_;
};

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be
 * started and stopped
 */
TEST_P(BluetoothAudioProviderLeAudioBroadcastHardwareAidl,
       OpenLeAudioOutputHardwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be
 * started and stopped with broadcast hardware encoding config taken from
 * provider info
 */
TEST_P(
    BluetoothAudioProviderLeAudioBroadcastHardwareAidl,
    StartAndEndLeAudioBroadcastSessionWithPossibleUnicastConfigFromProviderInfo) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }
  if (!IsBroadcastOffloadProviderInfoSupported()) {
    return;
  }

  auto lc3_codec_configs = GetBroadcastLc3SupportedListFromProviderInfo();
  LeAudioBroadcastConfiguration le_audio_broadcast_config = {
      .codecType = CodecType::LC3,
      .streamMap = {},
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_broadcast_config.streamMap.resize(1);
    le_audio_broadcast_config.streamMap[0]
        .leAudioCodecConfig.set<LeAudioCodecConfiguration::lc3Config>(
            lc3_config);
    le_audio_broadcast_config.streamMap[0].streamHandle = 0x0;
    le_audio_broadcast_config.streamMap[0].pcmStreamId = 0x0;
    le_audio_broadcast_config.streamMap[0].audioChannelAllocation = 0x1 << 0;

    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_broadcast_config),
        latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

TEST_P(BluetoothAudioProviderLeAudioBroadcastHardwareAidl,
       GetEmptyBroadcastConfigurationEmptyCapability) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsBroadcastOffloadSupported()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> empty_capability;
  IBluetoothAudioProvider::LeAudioBroadcastConfigurationRequirement
      empty_requirement;

  IBluetoothAudioProvider::LeAudioBroadcastConfigurationSetting configuration;

  // Check empty capability for source direction
  auto aidl_retval = audio_provider_->getLeAudioBroadcastConfiguration(
      empty_capability, empty_requirement, &configuration);

  ASSERT_FALSE(aidl_retval.isOk());
}

TEST_P(BluetoothAudioProviderLeAudioBroadcastHardwareAidl,
       GetBroadcastConfigurationEmptyCapability) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsBroadcastOffloadSupported()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> empty_capability;
  IBluetoothAudioProvider::LeAudioBroadcastConfigurationSetting configuration;

  IBluetoothAudioProvider::LeAudioBroadcastConfigurationRequirement
      one_subgroup_requirement =
          GetBroadcastRequirement(true /* standard*/, false /* high */);

  // Check empty capability for source direction
  auto aidl_retval = audio_provider_->getLeAudioBroadcastConfiguration(
      empty_capability, one_subgroup_requirement, &configuration);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_NE(configuration.numBis, 0);
  ASSERT_FALSE(configuration.subgroupsConfigurations.empty());
  VerifyBroadcastConfiguration(one_subgroup_requirement, configuration);
}

TEST_P(BluetoothAudioProviderLeAudioBroadcastHardwareAidl,
       GetBroadcastConfigurationNonEmptyCapability) {
  if (GetProviderFactoryInterfaceVersion() <
      BluetoothAudioHalVersion::VERSION_AIDL_V4) {
    GTEST_SKIP();
  }

  if (!IsBroadcastOffloadSupported()) {
    GTEST_SKIP();
  }

  std::vector<std::optional<LeAudioDeviceCapabilities>> capability = {
      GetDefaultBroadcastSinkCapability()};

  IBluetoothAudioProvider::LeAudioBroadcastConfigurationRequirement
      requirement =
          GetBroadcastRequirement(true /* standard*/, false /* high */);

  IBluetoothAudioProvider::LeAudioBroadcastConfigurationSetting configuration;

  // Check empty capability for source direction
  auto aidl_retval = audio_provider_->getLeAudioBroadcastConfiguration(
      capability, requirement, &configuration);

  ASSERT_TRUE(aidl_retval.isOk());
  ASSERT_NE(configuration.numBis, 0);
  ASSERT_FALSE(configuration.subgroupsConfigurations.empty());
  VerifyBroadcastConfiguration(requirement, configuration);
}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be
 * started and stopped with broadcast hardware encoding config
 */
TEST_P(BluetoothAudioProviderLeAudioBroadcastHardwareAidl,
       StartAndEndLeAudioBroadcastSessionWithPossibleBroadcastConfig) {
  if (!IsBroadcastOffloadSupported()) {
    GTEST_SKIP();
  }

  auto lc3_codec_configs = GetBroadcastLc3SupportedList(true /* supported */);
  LeAudioBroadcastConfiguration le_audio_broadcast_config = {
      .codecType = CodecType::LC3,
      .streamMap = {},
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_broadcast_config.streamMap.resize(1);
    le_audio_broadcast_config.streamMap[0]
        .leAudioCodecConfig.set<LeAudioCodecConfiguration::lc3Config>(
            lc3_config);
    le_audio_broadcast_config.streamMap[0].streamHandle = 0x0;
    le_audio_broadcast_config.streamMap[0].pcmStreamId = 0x0;
    le_audio_broadcast_config.streamMap[0].audioChannelAllocation = 0x1 << 0;

    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_broadcast_config),
        latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH can be
 * started and stopped with Broadcast hardware encoding config
 *
 * Disabled since offload codec checking is not ready
 */
TEST_P(
    BluetoothAudioProviderLeAudioBroadcastHardwareAidl,
    DISABLED_StartAndEndLeAudioBroadcastSessionWithInvalidAudioConfiguration) {
  if (!IsBroadcastOffloadSupported()) {
    GTEST_SKIP();
  }

  auto lc3_codec_configs = GetBroadcastLc3SupportedList(false /* supported */);
  LeAudioBroadcastConfiguration le_audio_broadcast_config = {
      .codecType = CodecType::LC3,
      .streamMap = {},
  };

  for (auto& lc3_config : lc3_codec_configs) {
    le_audio_broadcast_config.streamMap[0]
        .leAudioCodecConfig.set<LeAudioCodecConfiguration::lc3Config>(
            lc3_config);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(le_audio_broadcast_config),
        latency_modes, &mq_desc);

    // AIDL call should fail on invalid codec
    ASSERT_FALSE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * openProvider A2DP_SOFTWARE_DECODING_DATAPATH
 */
class BluetoothAudioProviderA2dpDecodingSoftwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(SessionType::A2DP_SOFTWARE_DECODING_DATAPATH);
    OpenProviderHelper(SessionType::A2DP_SOFTWARE_DECODING_DATAPATH);
    ASSERT_TRUE(temp_provider_capabilities_.empty() ||
                audio_provider_ != nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }
};

/**
 * Test whether we can open a provider of type
 */
TEST_P(BluetoothAudioProviderA2dpDecodingSoftwareAidl,
       OpenA2dpDecodingSoftwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::A2DP_SOFTWARE_DECODING_DATAPATH can be started and stopped
 * with different PCM config
 */
TEST_P(BluetoothAudioProviderA2dpDecodingSoftwareAidl,
       StartAndEndA2dpDecodingSoftwareSessionWithPossiblePcmConfig) {
  for (auto sample_rate : a2dp_sample_rates) {
    for (auto bits_per_sample : a2dp_bits_per_samples) {
      for (auto channel_mode : a2dp_channel_modes) {
        PcmConfiguration pcm_config{
            .sampleRateHz = sample_rate,
            .channelMode = channel_mode,
            .bitsPerSample = bits_per_sample,
        };
        bool is_codec_config_valid = IsPcmConfigSupported(pcm_config);
        DataMQDesc mq_desc;
        auto aidl_retval = audio_provider_->startSession(
            audio_port_, AudioConfiguration(pcm_config), latency_modes,
            &mq_desc);
        DataMQ data_mq(mq_desc);

        EXPECT_EQ(aidl_retval.isOk(), is_codec_config_valid);
        if (is_codec_config_valid) {
          EXPECT_TRUE(data_mq.isValid());
        }
        EXPECT_TRUE(audio_provider_->endSession().isOk());
      }
    }
  }
}

/**
 * openProvider A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH
 */
class BluetoothAudioProviderA2dpDecodingHardwareAidl
    : public BluetoothAudioProviderFactoryAidl {
 public:
  virtual void SetUp() override {
    BluetoothAudioProviderFactoryAidl::SetUp();
    GetProviderCapabilitiesHelper(
        SessionType::A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH);
    OpenProviderHelper(SessionType::A2DP_HARDWARE_OFFLOAD_DECODING_DATAPATH);
    ASSERT_TRUE(temp_provider_capabilities_.empty() ||
                audio_provider_ != nullptr);
  }

  virtual void TearDown() override {
    audio_port_ = nullptr;
    audio_provider_ = nullptr;
    BluetoothAudioProviderFactoryAidl::TearDown();
  }

  bool IsOffloadSupported() { return (temp_provider_capabilities_.size() > 0); }
};

/**
 * Test whether we can open a provider of type
 */
TEST_P(BluetoothAudioProviderA2dpDecodingHardwareAidl,
       OpenA2dpDecodingHardwareProvider) {}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_DECODING_DATAPATH can be started and stopped
 * with SBC hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpDecodingHardwareAidl,
       StartAndEndA2dpSbcDecodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  CodecConfiguration codec_config = {
      .codecType = CodecType::SBC,
      .encodedAudioBitrate = 328000,
      .peerMtu = 1005,
      .isScmstEnabled = false,
  };
  auto sbc_codec_specifics = GetSbcCodecSpecificSupportedList(true);

  for (auto& codec_specific : sbc_codec_specifics) {
    copy_codec_specific(codec_config.config, codec_specific);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(codec_config), latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_DECODING_DATAPATH can be started and stopped
 * with AAC hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpDecodingHardwareAidl,
       StartAndEndA2dpAacDecodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  CodecConfiguration codec_config = {
      .codecType = CodecType::AAC,
      .encodedAudioBitrate = 320000,
      .peerMtu = 1005,
      .isScmstEnabled = false,
  };
  auto aac_codec_specifics = GetAacCodecSpecificSupportedList(true);

  for (auto& codec_specific : aac_codec_specifics) {
    copy_codec_specific(codec_config.config, codec_specific);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(codec_config), latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_DECODING_DATAPATH can be started and stopped
 * with LDAC hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpDecodingHardwareAidl,
       StartAndEndA2dpLdacDecodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  CodecConfiguration codec_config = {
      .codecType = CodecType::LDAC,
      .encodedAudioBitrate = 990000,
      .peerMtu = 1005,
      .isScmstEnabled = false,
  };
  auto ldac_codec_specifics = GetLdacCodecSpecificSupportedList(true);

  for (auto& codec_specific : ldac_codec_specifics) {
    copy_codec_specific(codec_config.config, codec_specific);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(codec_config), latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_DECODING_DATAPATH can be started and stopped
 * with Opus hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpDecodingHardwareAidl,
       StartAndEndA2dpOpusDecodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  CodecConfiguration codec_config = {
      .codecType = CodecType::OPUS,
      .encodedAudioBitrate = 990000,
      .peerMtu = 1005,
      .isScmstEnabled = false,
  };
  auto opus_codec_specifics = GetOpusCodecSpecificSupportedList(true);

  for (auto& codec_specific : opus_codec_specifics) {
    copy_codec_specific(codec_config.config, codec_specific);
    DataMQDesc mq_desc;
    auto aidl_retval = audio_provider_->startSession(
        audio_port_, AudioConfiguration(codec_config), latency_modes, &mq_desc);

    ASSERT_TRUE(aidl_retval.isOk());
    EXPECT_TRUE(audio_provider_->endSession().isOk());
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_DECODING_DATAPATH can be started and stopped
 * with AptX hardware encoding config
 */
TEST_P(BluetoothAudioProviderA2dpDecodingHardwareAidl,
       StartAndEndA2dpAptxDecodingHardwareSession) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }

  for (auto codec_type : {CodecType::APTX, CodecType::APTX_HD}) {
    CodecConfiguration codec_config = {
        .codecType = codec_type,
        .encodedAudioBitrate =
            (codec_type == CodecType::APTX ? 352000 : 576000),
        .peerMtu = 1005,
        .isScmstEnabled = false,
    };

    auto aptx_codec_specifics = GetAptxCodecSpecificSupportedList(
        (codec_type == CodecType::APTX_HD ? true : false), true);

    for (auto& codec_specific : aptx_codec_specifics) {
      copy_codec_specific(codec_config.config, codec_specific);
      DataMQDesc mq_desc;
      auto aidl_retval = audio_provider_->startSession(
          audio_port_, AudioConfiguration(codec_config), latency_modes,
          &mq_desc);

      ASSERT_TRUE(aidl_retval.isOk());
      EXPECT_TRUE(audio_provider_->endSession().isOk());
    }
  }
}

/**
 * Test whether each provider of type
 * SessionType::A2DP_HARDWARE_DECODING_DATAPATH can be started and stopped
 * with an invalid codec config
 */
TEST_P(BluetoothAudioProviderA2dpDecodingHardwareAidl,
       StartAndEndA2dpDecodingHardwareSessionInvalidCodecConfig) {
  if (!IsOffloadSupported()) {
    GTEST_SKIP();
  }
  ASSERT_NE(audio_provider_, nullptr);

  std::vector<CodecConfiguration::CodecSpecific> codec_specifics;
  for (auto codec_type : ndk::enum_range<CodecType>()) {
    switch (codec_type) {
      case CodecType::SBC:
        codec_specifics = GetSbcCodecSpecificSupportedList(false);
        break;
      case CodecType::AAC:
        codec_specifics = GetAacCodecSpecificSupportedList(false);
        break;
      case CodecType::LDAC:
        codec_specifics = GetLdacCodecSpecificSupportedList(false);
        break;
      case CodecType::APTX:
        codec_specifics = GetAptxCodecSpecificSupportedList(false, false);
        break;
      case CodecType::APTX_HD:
        codec_specifics = GetAptxCodecSpecificSupportedList(true, false);
        break;
      case CodecType::OPUS:
        codec_specifics = GetOpusCodecSpecificSupportedList(false);
        continue;
      case CodecType::APTX_ADAPTIVE:
      case CodecType::APTX_ADAPTIVE_LE:
      case CodecType::APTX_ADAPTIVE_LEX:
      case CodecType::LC3:
      case CodecType::VENDOR:
      case CodecType::UNKNOWN:
        codec_specifics.clear();
        break;
    }
    if (codec_specifics.empty()) {
      continue;
    }

    CodecConfiguration codec_config = {
        .codecType = codec_type,
        .encodedAudioBitrate = 328000,
        .peerMtu = 1005,
        .isScmstEnabled = false,
    };
    for (auto codec_specific : codec_specifics) {
      copy_codec_specific(codec_config.config, codec_specific);
      DataMQDesc mq_desc;
      auto aidl_retval = audio_provider_->startSession(
          audio_port_, AudioConfiguration(codec_config), latency_modes,
          &mq_desc);

      // AIDL call should fail on invalid codec
      ASSERT_FALSE(aidl_retval.isOk());
      EXPECT_TRUE(audio_provider_->endSession().isOk());
    }
  }
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderFactoryAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance, BluetoothAudioProviderFactoryAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(BluetoothAudioProviderAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance, BluetoothAudioProviderAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderA2dpEncodingSoftwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderA2dpEncodingSoftwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderA2dpEncodingHardwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderA2dpEncodingHardwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderHearingAidSoftwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderHearingAidSoftwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderLeAudioOutputSoftwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderLeAudioOutputSoftwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderLeAudioInputSoftwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderLeAudioInputSoftwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderLeAudioOutputHardwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderLeAudioOutputHardwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderLeAudioInputHardwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderLeAudioInputHardwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderLeAudioBroadcastSoftwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderLeAudioBroadcastSoftwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderLeAudioBroadcastHardwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderLeAudioBroadcastHardwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderA2dpDecodingSoftwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderA2dpDecodingSoftwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderA2dpDecodingHardwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderA2dpDecodingHardwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderHfpHardwareAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance, BluetoothAudioProviderHfpHardwareAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderHfpSoftwareDecodingAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderHfpSoftwareDecodingAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(
    BluetoothAudioProviderHfpSoftwareEncodingAidl);
INSTANTIATE_TEST_SUITE_P(PerInstance,
                         BluetoothAudioProviderHfpSoftwareEncodingAidl,
                         testing::ValuesIn(android::getAidlHalInstanceNames(
                             IBluetoothAudioProviderFactory::descriptor)),
                         android::PrintInstanceNameToString);

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ABinderProcess_setThreadPoolMaxThreadCount(1);
  ABinderProcess_startThreadPool();
  return RUN_ALL_TESTS();
}
