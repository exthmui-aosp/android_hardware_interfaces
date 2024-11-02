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
#define LOG_TAG "ambient_backlight_aidl_hal_test"

#include <aidl/Gtest.h>
#include <aidl/Vintf.h>
#include <aidl/android/hardware/tv/mediaquality/AmbientBacklightEvent.h>
#include <aidl/android/hardware/tv/mediaquality/AmbientBacklightSettings.h>
#include <aidl/android/hardware/tv/mediaquality/BnMediaQualityCallback.h>
#include <aidl/android/hardware/tv/mediaquality/IMediaQuality.h>
#include <android/binder_auto_utils.h>
#include <android/binder_manager.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <future>

using aidl::android::hardware::graphics::common::PixelFormat;
using aidl::android::hardware::tv::mediaquality::AmbientBacklightEvent;
using aidl::android::hardware::tv::mediaquality::AmbientBacklightSettings;
using aidl::android::hardware::tv::mediaquality::AmbientBacklightSource;
using aidl::android::hardware::tv::mediaquality::BnMediaQualityCallback;
using aidl::android::hardware::tv::mediaquality::IMediaQuality;
using android::ProcessState;
using android::String16;
using ndk::ScopedAStatus;
using ndk::SpAIBinder;

#define ASSERT_OK(ret) ASSERT_TRUE(ret.isOk())
#define EXPECT_OK(ret) EXPECT_TRUE(ret.isOk())

class MediaQualityCallback : public BnMediaQualityCallback {
  public:
    explicit MediaQualityCallback(
            const std::function<void(const AmbientBacklightEvent& event)>& on_hal_event_cb)
        : on_hal_event_cb_(on_hal_event_cb) {}
    ScopedAStatus notifyAmbientBacklightEvent(const AmbientBacklightEvent& event) override {
        on_hal_event_cb_(event);
        return ScopedAStatus::ok();
    }

  private:
    std::function<void(const AmbientBacklightEvent& event)> on_hal_event_cb_;
};

class MediaQualityAidl : public testing::TestWithParam<std::string> {
  public:
    virtual void SetUp() override {
        mediaquality = IMediaQuality::fromBinder(
                SpAIBinder(AServiceManager_waitForService(GetParam().c_str())));
        ASSERT_NE(mediaquality, nullptr);
    }
    std::shared_ptr<IMediaQuality> mediaquality;
};

TEST_P(MediaQualityAidl, TestSetAmbientBacklightDetectionEnabled) {
    std::promise<void> open_cb_promise;
    std::future<void> open_cb_future{open_cb_promise.get_future()};
    std::shared_ptr<MediaQualityCallback> callback =
            ndk::SharedRefBase::make<MediaQualityCallback>([&open_cb_promise](auto event) {
                EXPECT_EQ(event.getTag(), AmbientBacklightEvent::Tag::enabled);
                EXPECT_EQ(event.template get<AmbientBacklightEvent::Tag::enabled>(), true);
                open_cb_promise.set_value();
                return ScopedAStatus::ok();
            });
    ASSERT_OK(mediaquality->setCallback(callback));
    ASSERT_OK(mediaquality->setAmbientBacklightDetectionEnabled(true));
    std::chrono::milliseconds timeout{10000};
    EXPECT_EQ(open_cb_future.wait_for(timeout), std::future_status::ready);
}

TEST_P(MediaQualityAidl, TestGetAmbientBacklightDetectionEnabled) {
    bool enabled;
    ASSERT_OK(mediaquality->getAmbientBacklightDetectionEnabled(&enabled));
}

TEST_P(MediaQualityAidl, TestSetMediaQualityCallback) {
    std::shared_ptr<MediaQualityCallback> callback = ndk::SharedRefBase::make<MediaQualityCallback>(
            [](auto /* event */) { return ScopedAStatus::ok(); });
    ASSERT_OK(mediaquality->setCallback(callback));
}

TEST_P(MediaQualityAidl, TestSetAmbientBacklightDetector) {
    AmbientBacklightSettings in_settings = {
            .packageName = "com.android.mediaquality",
            .source = AmbientBacklightSource::VIDEO,
            .colorFormat = PixelFormat::RGB_888,
            .hZonesNumber = 32,
            .vZonesNumber = 20,
            .hasLetterbox = true,
            .threshold = 0,
    };
    ASSERT_OK(mediaquality->setAmbientBacklightDetector(in_settings));
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(MediaQualityAidl);

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
