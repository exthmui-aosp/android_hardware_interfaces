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

#define LOG_TAG "VtsHalEnvironmentalReverbTest"
#include <android-base/logging.h>
#include <audio_utils/power.h>
#include <audio_utils/primitives.h>
#include <system/audio.h>
#include <numeric>

#include "EffectHelper.h"

using namespace android;
using namespace aidl::android::hardware::audio::effect;
using aidl::android::hardware::audio::common::getChannelCount;
using android::hardware::audio::common::testing::detail::TestExecutionTracer;
using TagVectorPair = std::pair<EnvironmentalReverb::Tag, std::vector<int>>;
using TagValuePair = std::pair<EnvironmentalReverb::Tag, int>;

static constexpr int kMaxRoomLevel = 0;
static constexpr int kMinRoomLevel = -6000;
static constexpr int kMinRoomHfLevel = -4000;
static constexpr int kMinDecayTime = 0;
static constexpr int kMinHfRatio = 100;
static constexpr int kMinLevel = -6000;
static constexpr int kMinDensity = 0;
static constexpr int kMinDiffusion = 0;
static constexpr int kMinDelay = 0;

static const std::vector<TagVectorPair> kParamsIncreasingVector = {
        {EnvironmentalReverb::roomLevelMb, {-3500, -2800, -2100, -1400, -700, 0}},
        {EnvironmentalReverb::roomHfLevelMb, {-4000, -3200, -2400, -1600, -800, 0}},
        {EnvironmentalReverb::decayTimeMs, {400, 800, 1200, 1600, 2000}},
        {EnvironmentalReverb::decayHfRatioPm, {1000, 900, 800, 700}},
        {EnvironmentalReverb::levelMb, {-3500, -2800, -2100, -1400, -700, 0}},
};

static const TagVectorPair kDiffusionParam = {EnvironmentalReverb::diffusionPm,
                                              {100, 300, 500, 700, 900}};
static const TagVectorPair kDensityParam = {EnvironmentalReverb::densityPm,
                                            {0, 200, 400, 600, 800, 1000}};

static const std::vector<TagValuePair> kParamsMinimumValue = {
        {EnvironmentalReverb::roomLevelMb, kMinRoomLevel},
        {EnvironmentalReverb::decayTimeMs, kMinDecayTime},
        {EnvironmentalReverb::levelMb, kMinLevel}};

std::vector<std::pair<std::shared_ptr<IFactory>, Descriptor>> kDescPair;

using Maker = std::set<int> (*)();
static const std::array<Maker, static_cast<int>(EnvironmentalReverb::bypass) + 1>
        kTestValueSetMaker = {
                nullptr,
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::roomLevelMb>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::roomHfLevelMb>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::decayTimeMs>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::decayHfRatioPm>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
                nullptr,
                nullptr,
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::levelMb>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::delayMs>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::diffusionPm>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::densityPm>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
                []() -> std::set<int> {
                    return EffectHelper::getTestValueSet<EnvironmentalReverb, int,
                                                         Range::environmentalReverb,
                                                         EnvironmentalReverb::bypass>(
                            kDescPair, EffectHelper::expandTestValueBasic<int>);
                },
};

static std::vector<TagValuePair> buildSetAndGetTestParams() {
    std::vector<TagValuePair> valueTag;
    for (EnvironmentalReverb::Tag tag : ndk::enum_range<EnvironmentalReverb::Tag>()) {
        std::set<int> values;
        int intTag = static_cast<int>(tag);
        if (intTag <= static_cast<int>(EnvironmentalReverb::bypass) &&
            kTestValueSetMaker[intTag] != nullptr) {
            values = kTestValueSetMaker[intTag]();
        }

        for (const auto& value : values) {
            valueTag.push_back(std::make_pair(tag, value));
        }
    }

    return valueTag;
}

/**
 * Tests do the following:
 * - Testing parameter range supported by the effect. Range is verified with IEffect.getDescriptor()
 *   and range defined in the documentation.
 * - Validating the effect by comparing the outputs of the supported parameters.
 */

enum ParamName { DESCRIPTOR_INDEX, TAG_VALUE_PAIR };

class EnvironmentalReverbHelper : public EffectHelper {
  public:
    EnvironmentalReverbHelper(std::pair<std::shared_ptr<IFactory>, Descriptor> pair) {
        std::tie(mFactory, mDescriptor) = pair;
    }

    void SetUpReverb() {
        ASSERT_NE(nullptr, mFactory);
        ASSERT_NO_FATAL_FAILURE(create(mFactory, mEffect, mDescriptor));

        Parameter::Specific specific = getDefaultParamSpecific();
        Parameter::Common common = createParamCommon(
                0 /* session */, 1 /* ioHandle */, 44100 /* iSampleRate */, 44100 /* oSampleRate */,
                mFrameCount /* iFrameCount */, mFrameCount /* oFrameCount */);
        ASSERT_NO_FATAL_FAILURE(open(mEffect, common, specific, &ret, EX_NONE));
        ASSERT_NE(nullptr, mEffect);
    }

    void TearDownReverb() {
        ASSERT_NO_FATAL_FAILURE(close(mEffect));
        ASSERT_NO_FATAL_FAILURE(destroy(mFactory, mEffect));
    }

    Parameter::Specific getDefaultParamSpecific() {
        EnvironmentalReverb er =
                EnvironmentalReverb::make<EnvironmentalReverb::roomLevelMb>(kMaxRoomLevel);
        Parameter::Specific specific =
                Parameter::Specific::make<Parameter::Specific::environmentalReverb>(er);
        return specific;
    }

    bool isParamValid(EnvironmentalReverb env) {
        return isParameterValid<EnvironmentalReverb, Range::environmentalReverb>(env, mDescriptor);
    }

    Parameter createParam(EnvironmentalReverb env) {
        return Parameter::make<Parameter::specific>(
                Parameter::Specific::make<Parameter::Specific::environmentalReverb>(env));
    }

    void setAndVerifyParam(binder_exception_t expected, EnvironmentalReverb env,
                           EnvironmentalReverb::Tag tag) {
        auto expectedParam = createParam(env);

        EXPECT_STATUS(expected, mEffect->setParameter(expectedParam)) << expectedParam.toString();

        if (expected == EX_NONE) {
            auto erId = EnvironmentalReverb::Id::make<EnvironmentalReverb::Id::commonTag>(
                    EnvironmentalReverb::Tag(tag));

            auto id = Parameter::Id::make<Parameter::Id::environmentalReverbTag>(erId);

            // get parameter
            Parameter getParam;
            EXPECT_STATUS(EX_NONE, mEffect->getParameter(id, &getParam));
            EXPECT_EQ(expectedParam, getParam) << "\nexpectedParam:" << expectedParam.toString()
                                               << "\ngetParam:" << getParam.toString();
        }
    }

    bool isAuxiliary() {
        return mDescriptor.common.flags.type ==
               aidl::android::hardware::audio::effect::Flags::Type::AUXILIARY;
    }

    float computeOutputEnergy(const std::vector<float>& input, std::vector<float> output) {
        if (!isAuxiliary()) {
            // Extract auxiliary output
            for (size_t i = 0; i < output.size(); i++) {
                output[i] -= input[i];
            }
        }
        return audio_utils_compute_energy_mono(output.data(), AUDIO_FORMAT_PCM_FLOAT,
                                               output.size());
    }

    using Maker = EnvironmentalReverb (*)(int);

    static constexpr std::array<Maker, static_cast<int>(EnvironmentalReverb::bypass) + 1>
            kEnvironmentalReverbParamMaker = {
                    nullptr,
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::roomLevelMb>(value);
                    },
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::roomHfLevelMb>(value);
                    },
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::decayTimeMs>(value);
                    },
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::decayHfRatioPm>(
                                value);
                    },
                    nullptr,
                    nullptr,
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::levelMb>(value);
                    },
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::delayMs>(value);
                    },
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::diffusionPm>(value);
                    },
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::densityPm>(value);
                    },
                    [](int value) -> EnvironmentalReverb {
                        return EnvironmentalReverb::make<EnvironmentalReverb::bypass>(value);
                    }};

    void createEnvParam(EnvironmentalReverb::Tag tag, int paramValue) {
        int intTag = static_cast<int>(tag);
        if (intTag <= static_cast<int>(EnvironmentalReverb::bypass) &&
            kEnvironmentalReverbParamMaker[intTag] != NULL) {
            mEnvParam = kEnvironmentalReverbParamMaker[intTag](paramValue);
        } else {
            GTEST_SKIP() << "Invalid parameter, skipping the test\n";
        }
    }

    void setParameterAndProcess(std::vector<float>& input, std::vector<float>& output, int val,
                                EnvironmentalReverb::Tag tag) {
        createEnvParam(tag, val);
        if (isParamValid(mEnvParam)) {
            ASSERT_NO_FATAL_FAILURE(setAndVerifyParam(EX_NONE, mEnvParam, tag));
            ASSERT_NO_FATAL_FAILURE(processAndWriteToOutput(input, output, mEffect, &ret));
        }
    }

    static constexpr int kDurationMilliSec = 500;
    static constexpr int kBufferSize = kSamplingFrequency * kDurationMilliSec / 1000;
    static constexpr int kInputFrequency = 2000;
    static constexpr int mChannelLayout = AudioChannelLayout::LAYOUT_STEREO;

    int mStereoChannelCount = getChannelCount(
            AudioChannelLayout::make<AudioChannelLayout::layoutMask>(mChannelLayout));
    int mFrameCount = kBufferSize / mStereoChannelCount;

    std::shared_ptr<IFactory> mFactory;
    std::shared_ptr<IEffect> mEffect;
    IEffect::OpenEffectReturn ret;
    Descriptor mDescriptor;
    EnvironmentalReverb mEnvParam;
};

class EnvironmentalReverbParamTest
    : public ::testing::TestWithParam<
              std::tuple<std::pair<std::shared_ptr<IFactory>, Descriptor>, TagValuePair>>,
      public EnvironmentalReverbHelper {
  public:
    EnvironmentalReverbParamTest()
        : EnvironmentalReverbHelper(std::get<DESCRIPTOR_INDEX>(GetParam())) {
        std::tie(mTag, mParamValue) = std::get<TAG_VALUE_PAIR>(GetParam());
    }
    void SetUp() override { ASSERT_NO_FATAL_FAILURE(SetUpReverb()); }
    void TearDown() override { TearDownReverb(); }

    EnvironmentalReverb::Tag mTag;
    int mParamValue;
};

TEST_P(EnvironmentalReverbParamTest, SetAndGetParameter) {
    createEnvParam(mTag, mParamValue);
    ASSERT_NO_FATAL_FAILURE(setAndVerifyParam(
            isParamValid(mEnvParam) ? EX_NONE : EX_ILLEGAL_ARGUMENT, mEnvParam, mTag));
}

INSTANTIATE_TEST_SUITE_P(
        EnvironmentalReverbTest, EnvironmentalReverbParamTest,
        ::testing::Combine(
                testing::ValuesIn(kDescPair = EffectFactoryHelper::getAllEffectDescriptors(
                                          IFactory::descriptor, getEffectTypeUuidEnvReverb())),
                testing::ValuesIn(buildSetAndGetTestParams())),
        [](const testing::TestParamInfo<EnvironmentalReverbParamTest::ParamType>& info) {
            auto descriptor = std::get<DESCRIPTOR_INDEX>(info.param).second;
            auto tag = std::get<TAG_VALUE_PAIR>(info.param).first;
            auto val = std::get<TAG_VALUE_PAIR>(info.param).second;
            std::string name =
                    getPrefix(descriptor) + "_Tag_" + toString(tag) + std::to_string(val);
            std::replace_if(
                    name.begin(), name.end(), [](const char c) { return !std::isalnum(c); }, '_');
            return name;
        });
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(EnvironmentalReverbParamTest);

class EnvironmentalReverbDataTest
    : public ::testing::TestWithParam<
              std::tuple<std::pair<std::shared_ptr<IFactory>, Descriptor>, TagVectorPair>>,
      public EnvironmentalReverbHelper {
  public:
    EnvironmentalReverbDataTest()
        : EnvironmentalReverbHelper(std::get<DESCRIPTOR_INDEX>(GetParam())) {
        std::tie(mTag, mParamValues) = std::get<TAG_VALUE_PAIR>(GetParam());
        mInput.resize(kBufferSize);
    }
    void SetUp() override {
        SKIP_TEST_IF_DATA_UNSUPPORTED(mDescriptor.common.flags);
        ASSERT_NO_FATAL_FAILURE(
                generateSineWave(kInputFrequency, mInput, 1.0, kSamplingFrequency, mChannelLayout));
        ASSERT_NO_FATAL_FAILURE(SetUpReverb());
    }
    void TearDown() override {
        SKIP_TEST_IF_DATA_UNSUPPORTED(mDescriptor.common.flags);
        TearDownReverb();
    }

    void assertEnergyIncreasingWithParameter(bool bypass) {
        createEnvParam(EnvironmentalReverb::bypass, bypass);
        ASSERT_NO_FATAL_FAILURE(setAndVerifyParam(EX_NONE, mEnvParam, EnvironmentalReverb::bypass));
        float baseEnergy = 0;
        for (int val : mParamValues) {
            std::vector<float> output(kBufferSize);
            setParameterAndProcess(mInput, output, val, mTag);
            float energy = computeOutputEnergy(mInput, output);
            ASSERT_GT(energy, baseEnergy);
            baseEnergy = energy;
        }
    }

    void assertZeroEnergyWithBypass(bool bypass) {
        createEnvParam(EnvironmentalReverb::bypass, bypass);
        ASSERT_NO_FATAL_FAILURE(setAndVerifyParam(EX_NONE, mEnvParam, EnvironmentalReverb::bypass));
        for (int val : mParamValues) {
            std::vector<float> output(kBufferSize);
            setParameterAndProcess(mInput, output, val, mTag);
            float energy = computeOutputEnergy(mInput, output);
            ASSERT_EQ(energy, 0);
        }
    }

    EnvironmentalReverb::Tag mTag;
    std::vector<int> mParamValues;
    std::vector<float> mInput;
};

TEST_P(EnvironmentalReverbDataTest, IncreasingParamValue) {
    ASSERT_NO_FATAL_FAILURE(assertEnergyIncreasingWithParameter(false));
}

TEST_P(EnvironmentalReverbDataTest, WithBypassEnabled) {
    ASSERT_NO_FATAL_FAILURE(assertZeroEnergyWithBypass(true));
}

INSTANTIATE_TEST_SUITE_P(
        EnvironmentalReverbTest, EnvironmentalReverbDataTest,
        ::testing::Combine(
                testing::ValuesIn(kDescPair = EffectFactoryHelper::getAllEffectDescriptors(
                                          IFactory::descriptor, getEffectTypeUuidEnvReverb())),
                testing::ValuesIn(kParamsIncreasingVector)),
        [](const testing::TestParamInfo<EnvironmentalReverbDataTest::ParamType>& info) {
            auto descriptor = std::get<DESCRIPTOR_INDEX>(info.param).second;
            auto tag = std::get<TAG_VALUE_PAIR>(info.param).first;
            std::string name = getPrefix(descriptor) + "_Tag_" + toString(tag);
            return name;
        });

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(EnvironmentalReverbDataTest);

class EnvironmentalReverbMinimumParamTest
    : public ::testing::TestWithParam<
              std::tuple<std::pair<std::shared_ptr<IFactory>, Descriptor>, TagValuePair>>,
      public EnvironmentalReverbHelper {
  public:
    EnvironmentalReverbMinimumParamTest()
        : EnvironmentalReverbHelper(std::get<DESCRIPTOR_INDEX>(GetParam())) {
        std::tie(mTag, mValue) = std::get<TAG_VALUE_PAIR>(GetParam());
    }
    void SetUp() override {
        SKIP_TEST_IF_DATA_UNSUPPORTED(mDescriptor.common.flags);
        ASSERT_NO_FATAL_FAILURE(SetUpReverb());
        createEnvParam(EnvironmentalReverb::roomLevelMb, kMinRoomLevel);
        ASSERT_NO_FATAL_FAILURE(
                setAndVerifyParam(EX_NONE, mEnvParam, EnvironmentalReverb::roomLevelMb));
    }
    void TearDown() override {
        SKIP_TEST_IF_DATA_UNSUPPORTED(mDescriptor.common.flags);
        TearDownReverb();
    }

    EnvironmentalReverb::Tag mTag;
    int mValue;
};

TEST_P(EnvironmentalReverbMinimumParamTest, MinimumValueTest) {
    std::vector<float> input(kBufferSize);
    ASSERT_NO_FATAL_FAILURE(
            generateSineWave(kInputFrequency, input, 1.0, kSamplingFrequency, mChannelLayout));
    std::vector<float> output(kBufferSize);
    setParameterAndProcess(input, output, mValue, mTag);
    float energy = computeOutputEnergy(input, output);
    // No Auxiliary output for minimum param values
    ASSERT_EQ(energy, 0);
}

INSTANTIATE_TEST_SUITE_P(
        EnvironmentalReverbTest, EnvironmentalReverbMinimumParamTest,
        ::testing::Combine(
                testing::ValuesIn(kDescPair = EffectFactoryHelper::getAllEffectDescriptors(
                                          IFactory::descriptor, getEffectTypeUuidEnvReverb())),
                testing::ValuesIn(kParamsMinimumValue)),
        [](const testing::TestParamInfo<EnvironmentalReverbMinimumParamTest::ParamType>& info) {
            auto descriptor = std::get<DESCRIPTOR_INDEX>(info.param).second;
            auto tag = std::get<TAG_VALUE_PAIR>(info.param).first;
            auto val = std::get<TAG_VALUE_PAIR>(info.param).second;
            std::string name =
                    getPrefix(descriptor) + "_Tag_" + toString(tag) + std::to_string(val);
            std::replace_if(
                    name.begin(), name.end(), [](const char c) { return !std::isalnum(c); }, '_');
            return name;
        });

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(EnvironmentalReverbMinimumParamTest);

class EnvironmentalReverbDiffusionTest
    : public ::testing::TestWithParam<
              std::tuple<std::pair<std::shared_ptr<IFactory>, Descriptor>, TagVectorPair>>,
      public EnvironmentalReverbHelper {
  public:
    EnvironmentalReverbDiffusionTest()
        : EnvironmentalReverbHelper(std::get<DESCRIPTOR_INDEX>(GetParam())) {
        std::tie(mTag, mParamValues) = std::get<TAG_VALUE_PAIR>(GetParam());
        mInput.resize(kBufferSize);
    }
    void SetUp() override {
        SKIP_TEST_IF_DATA_UNSUPPORTED(mDescriptor.common.flags);
        ASSERT_NO_FATAL_FAILURE(
                generateSineWave(kInputFrequency, mInput, 1.0, kSamplingFrequency, mChannelLayout));
        ASSERT_NO_FATAL_FAILURE(SetUpReverb());
    }
    void TearDown() override {
        SKIP_TEST_IF_DATA_UNSUPPORTED(mDescriptor.common.flags);
        TearDownReverb();
    }

    float getMean(std::vector<float>& buffer) {
        return std::accumulate(buffer.begin(), buffer.end(), 0.0) / buffer.size();
    }

    float getVariance(std::vector<float>& buffer) {
        if (isAuxiliary()) {
            accumulate_float(buffer.data(), mInput.data(), buffer.size());
        }
        float mean = getMean(buffer);
        float squaredDeltas =
                std::accumulate(buffer.begin(), buffer.end(), 0.0,
                                [mean](float a, float b) { return a + pow(b - mean, 2); });

        return squaredDeltas / buffer.size();
    }

    EnvironmentalReverb::Tag mTag;
    std::vector<int> mParamValues;
    std::vector<float> mInput;
};

TEST_P(EnvironmentalReverbDiffusionTest, DecreasingVarianceTest) {
    std::vector<float> baseOutput(kBufferSize);
    setParameterAndProcess(mInput, baseOutput, kMinDiffusion, mTag);
    ASSERT_EQ(baseOutput.size(),
              static_cast<size_t>(mFrameCount) * static_cast<size_t>(mStereoChannelCount));
    float baseVariance = getVariance(baseOutput);
    for (int value : mParamValues) {
        std::vector<float> output(kBufferSize);
        setParameterAndProcess(mInput, output, value, mTag);
        ASSERT_EQ(output.size(),
                  static_cast<size_t>(mFrameCount) * static_cast<size_t>(mStereoChannelCount));
        float variance = getVariance(output);
        ASSERT_LT(variance, baseVariance);
        baseVariance = variance;
    }
}

INSTANTIATE_TEST_SUITE_P(
        EnvironmentalReverbTest, EnvironmentalReverbDiffusionTest,
        ::testing::Combine(
                testing::ValuesIn(kDescPair = EffectFactoryHelper::getAllEffectDescriptors(
                                          IFactory::descriptor, getEffectTypeUuidEnvReverb())),
                testing::Values(kDiffusionParam)),
        [](const testing::TestParamInfo<EnvironmentalReverbDiffusionTest::ParamType>& info) {
            auto descriptor = std::get<DESCRIPTOR_INDEX>(info.param).second;
            auto tag = std::get<TAG_VALUE_PAIR>(info.param).first;
            std::string name = getPrefix(descriptor) + "_Tag_" + toString(tag);
            return name;
        });

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(EnvironmentalReverbDiffusionTest);

enum ParamDensityTest { DESCRIPTOR, TAG_DENSITY_VALUE, PARAM_DENSITY_VALUE, IS_INPUT_MUTE };

class EnvironmentalReverbDensityTest
    : public ::testing::TestWithParam<std::tuple<std::pair<std::shared_ptr<IFactory>, Descriptor>,
                                                 EnvironmentalReverb::Tag, int, bool>>,
      public EnvironmentalReverbHelper {
  public:
    EnvironmentalReverbDensityTest() : EnvironmentalReverbHelper(std::get<DESCRIPTOR>(GetParam())) {
        mTag = std::get<TAG_DENSITY_VALUE>(GetParam());
        mParamValues = std::get<PARAM_DENSITY_VALUE>(GetParam());
        mIsInputMute = (std::get<IS_INPUT_MUTE>(GetParam()));
        mInput.resize(kBufferSize);
    }
    void SetUp() override {
        SKIP_TEST_IF_DATA_UNSUPPORTED(mDescriptor.common.flags);
        if (mIsInputMute) {
            std::fill(mInput.begin(), mInput.end(), 0);
        } else {
            ASSERT_NO_FATAL_FAILURE(generateSineWave(kInputFrequency, mInput, 1.0,
                                                     kSamplingFrequency, mChannelLayout));
        }
        ASSERT_NO_FATAL_FAILURE(SetUpReverb());
    }
    void TearDown() override {
        SKIP_TEST_IF_DATA_UNSUPPORTED(mDescriptor.common.flags);
        TearDownReverb();
    }

    EnvironmentalReverb::Tag mTag;
    int mParamValues;
    std::vector<float> mInput;
    bool mIsInputMute;
};

TEST_P(EnvironmentalReverbDensityTest, DensityOutput) {
    float inputRmse =
            audio_utils_compute_energy_mono(mInput.data(), AUDIO_FORMAT_PCM_FLOAT, mInput.size());

    std::vector<float> output(kBufferSize);
    setParameterAndProcess(mInput, output, mParamValues, mTag);

    if (isAuxiliary() && !mIsInputMute) {
        accumulate_float(output.data(), mInput.data(), output.size());
    }

    float outputRmse =
            audio_utils_compute_energy_mono(output.data(), AUDIO_FORMAT_PCM_FLOAT, output.size());
    if (inputRmse != 0) {
        EXPECT_GT(outputRmse, 0);
    } else {
        EXPECT_EQ(outputRmse, inputRmse);
    }
}

INSTANTIATE_TEST_SUITE_P(
        EnvironmentalReverbTest, EnvironmentalReverbDensityTest,
        ::testing::Combine(
                testing::ValuesIn(kDescPair = EffectFactoryHelper::getAllEffectDescriptors(
                                          IFactory::descriptor, getEffectTypeUuidEnvReverb())),
                testing::Values(kDensityParam.first), testing::ValuesIn(kDensityParam.second),
                testing::Bool()),
        [](const testing::TestParamInfo<EnvironmentalReverbDensityTest::ParamType>& info) {
            auto descriptor = std::get<DESCRIPTOR>(info.param).second;
            auto tag = std::get<TAG_DENSITY_VALUE>(info.param);
            auto value = std::get<PARAM_DENSITY_VALUE>(info.param);
            std::string isInputMute = std::to_string(std::get<IS_INPUT_MUTE>(info.param));
            std::string name = getPrefix(descriptor) + "_Tag_" + toString(tag) + "_Value_" +
                               std::to_string(value) + "_isInputMute_" + isInputMute;
            return name;
        });

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(EnvironmentalReverbDensityTest);

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::UnitTest::GetInstance()->listeners().Append(new TestExecutionTracer());
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();
    return RUN_ALL_TESTS();
}
