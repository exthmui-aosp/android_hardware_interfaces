/*
 * Copyright (C) 2024 The Android Open Source Project
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

#define LOG_TAG "ambient_intelligence_light_aidl_hal_test"

#include <aidl/Gtest.h>
#include <aidl/Vintf.h>
#include <android/hardware/tv/mediaquality/IMediaQuality.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <gtest/gtest.h>
#include <hidl/GtestPrinter.h>
#include <hidl/ServiceManagement.h>

using android::ProcessState;
using android::sp;
using android::String16;
using android::binder::Status;
using android::hardware::hidl_vec;
using android::hardware::Return;
using android::hardware::Void;
using android::hardware::tv::mediaquality::IMediaQuality;

#define ASSERT_OK(ret) ASSERT_TRUE(ret.isOk())
#define EXPECT_OK(ret) EXPECT_TRUE(ret.isOk())

class MediaQualityAidl : public testing::TestWithParam<std::string> {
  public:
    void SetUp() override {
        mediaquality = android::waitForDeclaredService<IMediaQuality>(String16(GetParam().c_str()));
        ASSERT_NE(mediaquality, nullptr);
    }

    sp<IMediaQuality> mediaquality;

    void TearDown() override {}
};

TEST_P(MediaQualityAidl, TestSetAmbientLightDetectionEnabled) {
    ASSERT_OK(mediaquality->setAmbientLightDetectionEnabled(true));
}

TEST_P(MediaQualityAidl, TestGetAmbientLightDetectionEnabled) {
    bool enabled;
    Status status = mediaquality->getAmbientLightDetectionEnabled(&enabled);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(enabled);
}

INSTANTIATE_TEST_SUITE_P(
        PerInstance, MediaQualityAidl,
        testing::ValuesIn(android::getAidlHalInstanceNames(IMediaQuality::descriptor)),
        android::PrintInstanceNameToString);

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ProcessState::self()->setThreadPoolMaxThreadCount(1);
    ProcessState::self()->startThreadPool();
    return RUN_ALL_TESTS();
}
