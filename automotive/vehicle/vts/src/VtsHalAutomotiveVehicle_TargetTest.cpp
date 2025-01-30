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

#define LOG_TAG "VtsHalAutomotiveVehicle"

#include <AccessForVehicleProperty.h>
#include <ChangeModeForVehicleProperty.h>
#include <IVhalClient.h>
#include <VehicleHalTypes.h>
#include <VehicleUtils.h>
#include <VersionForVehicleProperty.h>
#include <aidl/Gtest.h>
#include <aidl/Vintf.h>
#include <aidl/android/hardware/automotive/vehicle/IVehicle.h>
#include <android-base/stringprintf.h>
#include <android-base/thread_annotations.h>
#include <android/binder_process.h>
#include <android/hardware/automotive/vehicle/2.0/IVehicle.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <hidl/GtestPrinter.h>
#include <hidl/ServiceManagement.h>
#include <inttypes.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using ::aidl::android::hardware::automotive::vehicle::AllowedAccessForVehicleProperty;
using ::aidl::android::hardware::automotive::vehicle::ChangeModeForVehicleProperty;
using ::aidl::android::hardware::automotive::vehicle::IVehicle;
using ::aidl::android::hardware::automotive::vehicle::StatusCode;
using ::aidl::android::hardware::automotive::vehicle::SubscribeOptions;
using ::aidl::android::hardware::automotive::vehicle::toString;
using ::aidl::android::hardware::automotive::vehicle::VehicleArea;
using ::aidl::android::hardware::automotive::vehicle::VehicleProperty;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyAccess;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyChangeMode;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyGroup;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyStatus;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyType;
using ::aidl::android::hardware::automotive::vehicle::VersionForVehicleProperty;
using ::android::getAidlHalInstanceNames;
using ::android::uptimeMillis;
using ::android::base::ScopedLockAssertion;
using ::android::base::StringPrintf;
using ::android::frameworks::automotive::vhal::ErrorCode;
using ::android::frameworks::automotive::vhal::HalPropError;
using ::android::frameworks::automotive::vhal::IHalAreaConfig;
using ::android::frameworks::automotive::vhal::IHalPropConfig;
using ::android::frameworks::automotive::vhal::IHalPropValue;
using ::android::frameworks::automotive::vhal::ISubscriptionCallback;
using ::android::frameworks::automotive::vhal::IVhalClient;
using ::android::frameworks::automotive::vhal::SubscribeOptionsBuilder;
using ::android::frameworks::automotive::vhal::VhalClientResult;
using ::android::hardware::getAllHalInstanceNames;
using ::android::hardware::Sanitize;
using ::android::hardware::automotive::vehicle::isSystemProp;
using ::android::hardware::automotive::vehicle::propIdToString;
using ::android::hardware::automotive::vehicle::toInt;
using ::testing::Ge;

constexpr int32_t kInvalidProp = 0x31600207;
// The timeout for retrying getting prop value after setting prop value.
constexpr int64_t kRetryGetPropAfterSetPropTimeoutMillis = 10'000;

struct ServiceDescriptor {
    std::string name;
    bool isAidlService;
};

struct PropertyConfigTestParam {
    VehicleProperty propId;
    std::vector<VehiclePropertyAccess> accessModes;
    VehiclePropertyChangeMode changeMode;
};

class VtsVehicleCallback final : public ISubscriptionCallback {
  private:
    std::mutex mLock;
    std::unordered_map<int32_t, std::vector<std::unique_ptr<IHalPropValue>>> mEvents
            GUARDED_BY(mLock);
    std::condition_variable mEventCond;

  public:
    void onPropertyEvent(const std::vector<std::unique_ptr<IHalPropValue>>& values) override {
        {
            std::lock_guard<std::mutex> lockGuard(mLock);
            for (auto& value : values) {
                int32_t propId = value->getPropId();
                mEvents[propId].push_back(value->clone());
            }
        }
        mEventCond.notify_one();
    }

    void onPropertySetError([[maybe_unused]] const std::vector<HalPropError>& errors) override {
        // Do nothing.
    }

    template <class Rep, class Period>
    bool waitForExpectedEvents(int32_t propId, size_t expectedEvents,
                               const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> uniqueLock(mLock);
        return mEventCond.wait_for(uniqueLock, timeout, [this, propId, expectedEvents] {
            ScopedLockAssertion lockAssertion(mLock);
            return mEvents[propId].size() >= expectedEvents;
        });
    }

    std::vector<std::unique_ptr<IHalPropValue>> getEvents(int32_t propId) {
        std::lock_guard<std::mutex> lockGuard(mLock);
        std::vector<std::unique_ptr<IHalPropValue>> events;
        if (mEvents.find(propId) == mEvents.end()) {
            return events;
        }
        for (const auto& eventPtr : mEvents[propId]) {
            events.push_back(eventPtr->clone());
        }
        return events;
    }

    std::vector<int64_t> getEventTimestamps(int32_t propId) {
        std::lock_guard<std::mutex> lockGuard(mLock);
        std::vector<int64_t> timestamps;
        if (mEvents.find(propId) == mEvents.end()) {
            return timestamps;
        }
        for (const auto& valuePtr : mEvents[propId]) {
            timestamps.push_back(valuePtr->getTimestamp());
        }
        return timestamps;
    }

    void reset() {
        std::lock_guard<std::mutex> lockGuard(mLock);
        mEvents.clear();
    }
};

class VtsHalAutomotiveTest : public testing::Test {
  public:
    void verifyAccessMode(int actualAccess, std::vector<VehiclePropertyAccess> expectedAccess);
    void verifyGlobalAccessIsMaximalAreaAccessSubset(
            int propertyLevelAccess,
            const std::vector<std::unique_ptr<IHalAreaConfig>>& areaConfigs) const;
    void verifyProperty(VehicleProperty propId, std::vector<VehiclePropertyAccess> accessModes,
                        VehiclePropertyChangeMode changeMode);

    static bool isBooleanGlobalProp(int32_t property) {
        return (property & toInt(VehiclePropertyType::MASK)) ==
                       toInt(VehiclePropertyType::BOOLEAN) &&
               (property & toInt(VehicleArea::MASK)) == toInt(VehicleArea::GLOBAL);
    }

  protected:
    std::shared_ptr<IVhalClient> mVhalClient;
    std::shared_ptr<VtsVehicleCallback> mCallback;

    bool checkIsSupported(int32_t propertyId);
    void connectToVhal(const ServiceDescriptor& descriptor);

    static bool isUnavailable(const VhalClientResult<std::unique_ptr<IHalPropValue>>& result);
    static bool isResultOkayWithValue(
            const VhalClientResult<std::unique_ptr<IHalPropValue>>& result, int32_t value);
};

bool VtsHalAutomotiveTest::checkIsSupported(int32_t propertyId) {
    auto result = mVhalClient->getPropConfigs({propertyId});
    return result.ok();
}

void VtsHalAutomotiveTest::connectToVhal(const ServiceDescriptor& descriptor) {
    if (descriptor.isAidlService) {
        mVhalClient = IVhalClient::tryCreateAidlClient(descriptor.name.c_str());
    } else {
        mVhalClient = IVhalClient::tryCreateHidlClient(descriptor.name.c_str());
    }

    ASSERT_NE(mVhalClient, nullptr) << "Failed to connect to VHAL";

    mCallback = std::make_shared<VtsVehicleCallback>();
}

bool VtsHalAutomotiveTest::isResultOkayWithValue(
        const VhalClientResult<std::unique_ptr<IHalPropValue>>& result, int32_t value) {
    return result.ok() && result.value() != nullptr &&
           result.value()->getStatus() == VehiclePropertyStatus::AVAILABLE &&
           result.value()->getInt32Values().size() == 1 &&
           result.value()->getInt32Values()[0] == value;
}

bool VtsHalAutomotiveTest::isUnavailable(
        const VhalClientResult<std::unique_ptr<IHalPropValue>>& result) {
    if (!result.ok()) {
        return result.error().code() == ErrorCode::NOT_AVAILABLE_FROM_VHAL;
    }
    if (result.value() != nullptr &&
        result.value()->getStatus() == VehiclePropertyStatus::UNAVAILABLE) {
        return true;
    }

    return false;
}

void VtsHalAutomotiveTest::verifyAccessMode(int actualAccess,
                                            std::vector<VehiclePropertyAccess> expectedAccess) {
    if (actualAccess == toInt(VehiclePropertyAccess::NONE)) {
        return;
    }
    EXPECT_THAT(expectedAccess,
                ::testing::Contains(static_cast<VehiclePropertyAccess>(actualAccess)))
            << "Invalid property access mode, not one of the allowed access modes";
}

void VtsHalAutomotiveTest::verifyGlobalAccessIsMaximalAreaAccessSubset(
        int propertyLevelAccess,
        const std::vector<std::unique_ptr<IHalAreaConfig>>& areaConfigs) const {
    bool readOnlyPresent = false;
    bool writeOnlyPresent = false;
    bool readWritePresent = false;
    int maximalAreaAccessSubset = toInt(VehiclePropertyAccess::NONE);
    for (size_t i = 0; i < areaConfigs.size(); i++) {
        int access = areaConfigs[i]->getAccess();
        switch (access) {
            case toInt(VehiclePropertyAccess::READ):
                readOnlyPresent = true;
                break;
            case toInt(VehiclePropertyAccess::WRITE):
                writeOnlyPresent = true;
                break;
            case toInt(VehiclePropertyAccess::READ_WRITE):
                readWritePresent = true;
                break;
            default:
                ASSERT_EQ(access, toInt(VehiclePropertyAccess::NONE)) << StringPrintf(
                        "Area access can be NONE only if global property access is also NONE");
                return;
        }
    }

    if (readOnlyPresent) {
        ASSERT_FALSE(writeOnlyPresent) << StringPrintf(
                "Found both READ_ONLY and WRITE_ONLY access modes in area configs, which is not "
                "supported");
        maximalAreaAccessSubset = toInt(VehiclePropertyAccess::READ);
    } else if (writeOnlyPresent) {
        ASSERT_FALSE(readWritePresent) << StringPrintf(
                "Found both WRITE_ONLY and READ_WRITE access modes in area configs, which is not "
                "supported");
        maximalAreaAccessSubset = toInt(VehiclePropertyAccess::WRITE);
    } else if (readWritePresent) {
        maximalAreaAccessSubset = toInt(VehiclePropertyAccess::READ_WRITE);
    }
    ASSERT_EQ(propertyLevelAccess, maximalAreaAccessSubset) << StringPrintf(
            "Expected global access to be equal to maximal area access subset %d, Instead got %d",
            maximalAreaAccessSubset, propertyLevelAccess);
}

// Helper function to compare actual vs expected property config
void VtsHalAutomotiveTest::verifyProperty(VehicleProperty propId,
                                          std::vector<VehiclePropertyAccess> accessModes,
                                          VehiclePropertyChangeMode changeMode) {
    int expectedPropId = toInt(propId);
    int expectedChangeMode = toInt(changeMode);

    auto result = mVhalClient->getAllPropConfigs();
    ASSERT_TRUE(result.ok()) << "Failed to get all property configs, error: "
                             << result.error().message();

    // Check if property is implemented by getting all configs and looking to see if the expected
    // property id is in that list.
    bool isExpectedPropIdImplemented = false;
    for (const auto& cfgPtr : result.value()) {
        const IHalPropConfig& cfg = *cfgPtr;
        if (expectedPropId == cfg.getPropId()) {
            isExpectedPropIdImplemented = true;
            break;
        }
    }

    if (!isExpectedPropIdImplemented) {
        GTEST_SKIP() << StringPrintf("Property %" PRId32 " has not been implemented",
                                     expectedPropId);
    }

    result = mVhalClient->getPropConfigs({expectedPropId});
    ASSERT_TRUE(result.ok()) << "Failed to get required property config, error: "
                             << result.error().message();

    ASSERT_EQ(result.value().size(), 1u)
            << StringPrintf("Expect to get exactly 1 config, got %zu", result.value().size());

    const auto& config = result.value().at(0);
    int actualPropId = config->getPropId();
    int actualChangeMode = config->getChangeMode();

    ASSERT_EQ(actualPropId, expectedPropId)
            << StringPrintf("Expect to get property ID: %i, got %i", expectedPropId, actualPropId);

    int globalAccess = config->getAccess();
    if (config->getAreaConfigSize() == 0) {
        verifyAccessMode(globalAccess, accessModes);
    } else {
        for (const auto& areaConfig : config->getAreaConfigs()) {
            int areaConfigAccess = areaConfig->getAccess();
            int actualAccess = (areaConfigAccess != toInt(VehiclePropertyAccess::NONE))
                                       ? areaConfigAccess
                                       : globalAccess;
            verifyAccessMode(actualAccess, accessModes);
        }
    }

    EXPECT_EQ(actualChangeMode, expectedChangeMode)
            << StringPrintf("Expect to get VehiclePropertyChangeMode: %i, got %i",
                            expectedChangeMode, actualChangeMode);
}

class VtsHalAutomotiveVehicleTargetTest : public VtsHalAutomotiveTest,
                                          public testing::WithParamInterface<ServiceDescriptor> {
    virtual void SetUp() override { ASSERT_NO_FATAL_FAILURE(connectToVhal(GetParam())); }
};

class VtsHalAutomotivePropertyConfigTest
    : public VtsHalAutomotiveTest,
      public testing::WithParamInterface<std::tuple<PropertyConfigTestParam, ServiceDescriptor>> {
    virtual void SetUp() override {
        ASSERT_NO_FATAL_FAILURE(connectToVhal(std::get<1>(GetParam())));
    }
};

TEST_P(VtsHalAutomotiveVehicleTargetTest, useAidlBackend) {
    if (!mVhalClient->isAidlVhal()) {
        GTEST_SKIP() << "AIDL backend is not available, HIDL backend is used instead";
    }
}

TEST_P(VtsHalAutomotiveVehicleTargetTest, useHidlBackend) {
    if (mVhalClient->isAidlVhal()) {
        GTEST_SKIP() << "AIDL backend is available, HIDL backend is not used";
    }
}

// Test getAllPropConfigs() returns at least 1 property configs.
TEST_P(VtsHalAutomotiveVehicleTargetTest, getAllPropConfigs) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::getAllPropConfigs");

    auto result = mVhalClient->getAllPropConfigs();

    ASSERT_TRUE(result.ok()) << "Failed to get all property configs, error: "
                             << result.error().message();
    ASSERT_GE(result.value().size(), 1u) << StringPrintf(
            "Expect to get at least 1 property config, got %zu", result.value().size());
}

// Test getPropConfigs() can query properties returned by getAllPropConfigs.
TEST_P(VtsHalAutomotiveVehicleTargetTest, getPropConfigsWithValidProps) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::getRequiredPropConfigs");

    std::vector<int32_t> properties;
    auto result = mVhalClient->getAllPropConfigs();

    ASSERT_TRUE(result.ok()) << "Failed to get all property configs, error: "
                             << result.error().message();
    for (const auto& cfgPtr : result.value()) {
        properties.push_back(cfgPtr->getPropId());
    }

    result = mVhalClient->getPropConfigs(properties);

    ASSERT_TRUE(result.ok()) << "Failed to get required property config, error: "
                             << result.error().message();
    ASSERT_EQ(result.value().size(), properties.size()) << StringPrintf(
            "Expect to get exactly %zu configs, got %zu", properties.size(), result.value().size());
}

// Test getPropConfig() with an invalid propertyId returns an error code.
TEST_P(VtsHalAutomotiveVehicleTargetTest, getPropConfigsWithInvalidProp) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::getPropConfigsWithInvalidProp");

    auto result = mVhalClient->getPropConfigs({kInvalidProp});

    ASSERT_FALSE(result.ok()) << StringPrintf(
            "Expect failure to get prop configs for invalid prop: %" PRId32, kInvalidProp);
    ASSERT_NE(result.error().message(), "") << "Expect error message not to be empty";
}

// Test system property IDs returned by getPropConfigs() are defined in the VHAL property interface.
TEST_P(VtsHalAutomotiveVehicleTargetTest, testPropConfigs_onlyDefinedSystemPropertyIdsReturned) {
    if (!mVhalClient->isAidlVhal()) {
        GTEST_SKIP() << "Skip for HIDL VHAL because HAL interface run-time version is only"
                     << "introduced for AIDL";
    }

    auto result = mVhalClient->getAllPropConfigs();
    ASSERT_TRUE(result.ok()) << "Failed to get all property configs, error: "
                             << result.error().message();

    int32_t vhalVersion = mVhalClient->getRemoteInterfaceVersion();
    const auto& configs = result.value();
    for (size_t i = 0; i < configs.size(); i++) {
        int32_t propId = configs[i]->getPropId();
        if (!isSystemProp(propId)) {
            continue;
        }

        std::string propName = propIdToString(propId);
        auto it = VersionForVehicleProperty.find(static_cast<VehicleProperty>(propId));
        bool found = (it != VersionForVehicleProperty.end());
        EXPECT_TRUE(found) << "System Property: " << propName
                           << " is not defined in VHAL property interface";
        if (!found) {
            continue;
        }
        int32_t requiredVersion = it->second;
        EXPECT_THAT(vhalVersion, Ge(requiredVersion))
                << "System Property: " << propName << " requires VHAL version: " << requiredVersion
                << ", but the current VHAL version"
                << " is " << vhalVersion << ", must not be supported";
    }
}

TEST_P(VtsHalAutomotiveVehicleTargetTest, testPropConfigs_globalAccessIsMaximalAreaAccessSubset) {
    if (!mVhalClient->isAidlVhal()) {
        GTEST_SKIP() << "Skip for HIDL VHAL because HAL interface run-time version is only"
                     << "introduced for AIDL";
    }

    auto result = mVhalClient->getAllPropConfigs();
    ASSERT_TRUE(result.ok()) << "Failed to get all property configs, error: "
                             << result.error().message();

    const auto& configs = result.value();
    for (size_t i = 0; i < configs.size(); i++) {
        verifyGlobalAccessIsMaximalAreaAccessSubset(configs[i]->getAccess(),
                                                    configs[i]->getAreaConfigs());
    }
}

// Test get() return current value for properties.
TEST_P(VtsHalAutomotiveVehicleTargetTest, get) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::get");

    int32_t propId = toInt(VehicleProperty::PERF_VEHICLE_SPEED);
    if (!checkIsSupported(propId)) {
        GTEST_SKIP() << "Property: " << propId << " is not supported, skip the test";
    }
    auto result = mVhalClient->getValueSync(*mVhalClient->createHalPropValue(propId));

    ASSERT_TRUE(result.ok()) << StringPrintf("Failed to get value for property: %" PRId32
                                             ", error: %s",
                                             propId, result.error().message().c_str());
    ASSERT_NE(result.value(), nullptr) << "Result value must not be null";
}

// Test get() with an invalid propertyId return an error codes.
TEST_P(VtsHalAutomotiveVehicleTargetTest, getInvalidProp) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::getInvalidProp");

    auto result = mVhalClient->getValueSync(*mVhalClient->createHalPropValue(kInvalidProp));

    ASSERT_FALSE(result.ok()) << StringPrintf(
            "Expect failure to get property for invalid prop: %" PRId32, kInvalidProp);
}

// Test set() on read_write properties.
TEST_P(VtsHalAutomotiveVehicleTargetTest, setProp) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::setProp");

    // skip hvac related properties
    std::unordered_set<int32_t> hvacProps = {toInt(VehicleProperty::HVAC_DEFROSTER),
                                             toInt(VehicleProperty::HVAC_AC_ON),
                                             toInt(VehicleProperty::HVAC_MAX_AC_ON),
                                             toInt(VehicleProperty::HVAC_MAX_DEFROST_ON),
                                             toInt(VehicleProperty::HVAC_RECIRC_ON),
                                             toInt(VehicleProperty::HVAC_DUAL_ON),
                                             toInt(VehicleProperty::HVAC_AUTO_ON),
                                             toInt(VehicleProperty::HVAC_POWER_ON),
                                             toInt(VehicleProperty::HVAC_AUTO_RECIRC_ON),
                                             toInt(VehicleProperty::HVAC_ELECTRIC_DEFROSTER_ON)};
    auto result = mVhalClient->getAllPropConfigs();
    ASSERT_TRUE(result.ok());

    for (const auto& cfgPtr : result.value()) {
        const IHalPropConfig& cfg = *cfgPtr;
        int32_t propId = cfg.getPropId();
        // test on boolean and writable property
        bool isReadWrite = (cfg.getAccess() == toInt(VehiclePropertyAccess::READ_WRITE));
        if (cfg.getAreaConfigSize() != 0 &&
            cfg.getAreaConfigs()[0]->getAccess() != toInt(VehiclePropertyAccess::NONE)) {
            isReadWrite = (cfg.getAreaConfigs()[0]->getAccess() ==
                           toInt(VehiclePropertyAccess::READ_WRITE));
        }
        if (isReadWrite && isBooleanGlobalProp(propId) && !hvacProps.count(propId)) {
            auto propToGet = mVhalClient->createHalPropValue(propId);
            auto getValueResult = mVhalClient->getValueSync(*propToGet);

            if (isUnavailable(getValueResult)) {
                ALOGW("getProperty for %" PRId32
                      " returns NOT_AVAILABLE, "
                      "skip testing setProp",
                      propId);
                return;
            }

            ASSERT_TRUE(getValueResult.ok())
                    << StringPrintf("Failed to get value for property: %" PRId32 ", error: %s",
                                    propId, getValueResult.error().message().c_str());
            ASSERT_NE(getValueResult.value(), nullptr)
                    << StringPrintf("Result value must not be null for property: %" PRId32, propId);

            const IHalPropValue& value = *getValueResult.value();
            size_t intValueSize = value.getInt32Values().size();
            ASSERT_EQ(intValueSize, 1u) << StringPrintf(
                    "Expect exactly 1 int value for boolean property: %" PRId32 ", got %zu", propId,
                    intValueSize);

            int32_t setValue = value.getInt32Values()[0] == 1 ? 0 : 1;
            auto propToSet = mVhalClient->createHalPropValue(propId);
            propToSet->setInt32Values({setValue});
            auto setValueResult = mVhalClient->setValueSync(*propToSet);

            if (!setValueResult.ok() &&
                setValueResult.error().code() == ErrorCode::NOT_AVAILABLE_FROM_VHAL) {
                ALOGW("setProperty for %" PRId32
                      " returns NOT_AVAILABLE, "
                      "skip verifying getProperty returns the same value",
                      propId);
                return;
            }

            ASSERT_TRUE(setValueResult.ok())
                    << StringPrintf("Failed to set value for property: %" PRId32 ", error: %s",
                                    propId, setValueResult.error().message().c_str());
            // Retry getting the value until we pass the timeout. getValue might not return
            // the expected value immediately since setValue is async.
            auto timeoutMillis = uptimeMillis() + kRetryGetPropAfterSetPropTimeoutMillis;

            while (true) {
                getValueResult = mVhalClient->getValueSync(*propToGet);
                if (isResultOkayWithValue(getValueResult, setValue)) {
                    break;
                }
                if (uptimeMillis() >= timeoutMillis) {
                    // Reach timeout, the following assert should fail.
                    break;
                }
                // Sleep for 100ms between each getValueSync retry.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (isUnavailable(getValueResult)) {
                ALOGW("getProperty for %" PRId32
                      " returns NOT_AVAILABLE, "
                      "skip verifying the return value",
                      propId);
                return;
            }

            ASSERT_TRUE(getValueResult.ok())
                    << StringPrintf("Failed to get value for property: %" PRId32 ", error: %s",
                                    propId, getValueResult.error().message().c_str());
            ASSERT_NE(getValueResult.value(), nullptr)
                    << StringPrintf("Result value must not be null for property: %" PRId32, propId);
            ASSERT_EQ(getValueResult.value()->getInt32Values(), std::vector<int32_t>({setValue}))
                    << StringPrintf("Boolean value not updated after set for property: %" PRId32,
                                    propId);
        }
    }
}

// Test set() on an read_only property.
TEST_P(VtsHalAutomotiveVehicleTargetTest, setNotWritableProp) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::setNotWritableProp");

    int32_t propId = toInt(VehicleProperty::PERF_VEHICLE_SPEED);
    if (!checkIsSupported(propId)) {
        GTEST_SKIP() << "Property: " << propId << " is not supported, skip the test";
    }

    auto getValueResult = mVhalClient->getValueSync(*mVhalClient->createHalPropValue(propId));
    ASSERT_TRUE(getValueResult.ok())
            << StringPrintf("Failed to get value for property: %" PRId32 ", error: %s", propId,
                            getValueResult.error().message().c_str());

    auto setValueResult = mVhalClient->setValueSync(*getValueResult.value());

    ASSERT_FALSE(setValueResult.ok()) << "Expect set a read-only value to fail";
    ASSERT_EQ(setValueResult.error().code(), ErrorCode::ACCESS_DENIED_FROM_VHAL);
}

// Test get(), set() and getAllPropConfigs() on VehicleProperty::INVALID.
TEST_P(VtsHalAutomotiveVehicleTargetTest, getSetPropertyIdInvalid) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::getSetPropertyIdInvalid");

    int32_t propId = toInt(VehicleProperty::INVALID);
    auto getValueResult = mVhalClient->getValueSync(*mVhalClient->createHalPropValue(propId));
    ASSERT_FALSE(getValueResult.ok()) << "Expect get on VehicleProperty::INVALID to fail";
    ASSERT_EQ(getValueResult.error().code(), ErrorCode::INVALID_ARG);

    auto propToSet = mVhalClient->createHalPropValue(propId);
    propToSet->setInt32Values({0});
    auto setValueResult = mVhalClient->setValueSync(*propToSet);
    ASSERT_FALSE(setValueResult.ok()) << "Expect set on VehicleProperty::INVALID to fail";
    ASSERT_EQ(setValueResult.error().code(), ErrorCode::INVALID_ARG);

    auto result = mVhalClient->getAllPropConfigs();
    ASSERT_TRUE(result.ok());
    for (const auto& cfgPtr : result.value()) {
        const IHalPropConfig& cfg = *cfgPtr;
        ASSERT_FALSE(cfg.getPropId() == propId) << "Expect VehicleProperty::INVALID to not be "
                                                   "included in propConfigs";
    }
}

// Test subscribe() and unsubscribe().
TEST_P(VtsHalAutomotiveVehicleTargetTest, subscribeAndUnsubscribe) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::subscribeAndUnsubscribe");

    int32_t propId = toInt(VehicleProperty::PERF_VEHICLE_SPEED);
    if (!checkIsSupported(propId)) {
        GTEST_SKIP() << "Property: " << propId << " is not supported, skip the test";
    }

    auto propConfigsResult = mVhalClient->getPropConfigs({propId});

    ASSERT_TRUE(propConfigsResult.ok()) << "Failed to get property config for PERF_VEHICLE_SPEED: "
                                        << "error: " << propConfigsResult.error().message();
    ASSERT_EQ(propConfigsResult.value().size(), 1u)
            << "Expect to return 1 config for PERF_VEHICLE_SPEED";
    auto& propConfig = propConfigsResult.value()[0];
    float minSampleRate = propConfig->getMinSampleRate();
    float maxSampleRate = propConfig->getMaxSampleRate();

    if (minSampleRate < 1) {
        GTEST_SKIP() << "Sample rate for vehicle speed < 1 times/sec, skip test since it would "
                        "take too long";
    }

    auto client = mVhalClient->getSubscriptionClient(mCallback);
    ASSERT_NE(client, nullptr) << "Failed to get subscription client";

    auto result = client->subscribe({{.propId = propId, .sampleRate = minSampleRate}});

    ASSERT_TRUE(result.ok()) << StringPrintf("Failed to subscribe to property: %" PRId32
                                             ", error: %s",
                                             propId, result.error().message().c_str());

    if (mVhalClient->isAidlVhal()) {
        // Skip checking timestamp for HIDL because the behavior for sample rate and timestamp is
        // only specified clearly for AIDL.

        // Timeout is 2 seconds, which gives a 1 second buffer.
        ASSERT_TRUE(mCallback->waitForExpectedEvents(propId, std::floor(minSampleRate),
                                                     std::chrono::seconds(2)))
                << "Didn't get enough events for subscribing to minSampleRate";
    }

    result = client->subscribe({{.propId = propId, .sampleRate = maxSampleRate}});

    ASSERT_TRUE(result.ok()) << StringPrintf("Failed to subscribe to property: %" PRId32
                                             ", error: %s",
                                             propId, result.error().message().c_str());

    if (mVhalClient->isAidlVhal()) {
        ASSERT_TRUE(mCallback->waitForExpectedEvents(propId, std::floor(maxSampleRate),
                                                     std::chrono::seconds(2)))
                << "Didn't get enough events for subscribing to maxSampleRate";

        std::unordered_set<int64_t> timestamps;
        // Event event should have a different timestamp.
        for (const int64_t& eventTimestamp : mCallback->getEventTimestamps(propId)) {
            ASSERT_TRUE(timestamps.find(eventTimestamp) == timestamps.end())
                    << "two events for the same property must not have the same timestamp";
            timestamps.insert(eventTimestamp);
        }
    }

    result = client->unsubscribe({propId});
    ASSERT_TRUE(result.ok()) << StringPrintf("Failed to unsubscribe to property: %" PRId32
                                             ", error: %s",
                                             propId, result.error().message().c_str());

    mCallback->reset();
    ASSERT_FALSE(mCallback->waitForExpectedEvents(propId, 10, std::chrono::seconds(1)))
            << "Expect not to get events after unsubscription";
}

bool isVariableUpdateRateSupported(const std::unique_ptr<IHalPropConfig>& config, int32_t areaId) {
    for (const auto& areaConfigPtr : config->getAreaConfigs()) {
        if (areaConfigPtr->getAreaId() == areaId &&
            areaConfigPtr->isVariableUpdateRateSupported()) {
            return true;
        }
    }
    return false;
}

// Test subscribe with variable update rate enabled if supported.
TEST_P(VtsHalAutomotiveVehicleTargetTest, subscribe_enableVurIfSupported) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::subscribe_enableVurIfSupported");

    int32_t propId = toInt(VehicleProperty::PERF_VEHICLE_SPEED);
    if (!checkIsSupported(propId)) {
        GTEST_SKIP() << "Property: " << propId << " is not supported, skip the test";
    }
    if (!mVhalClient->isAidlVhal()) {
        GTEST_SKIP() << "Variable update rate is only supported by AIDL VHAL";
    }

    auto propConfigsResult = mVhalClient->getPropConfigs({propId});

    ASSERT_TRUE(propConfigsResult.ok()) << "Failed to get property config for PERF_VEHICLE_SPEED: "
                                        << "error: " << propConfigsResult.error().message();
    ASSERT_EQ(propConfigsResult.value().size(), 1u)
            << "Expect to return 1 config for PERF_VEHICLE_SPEED";
    auto& propConfig = propConfigsResult.value()[0];
    float maxSampleRate = propConfig->getMaxSampleRate();
    if (maxSampleRate < 1) {
        GTEST_SKIP() << "Sample rate for vehicle speed < 1 times/sec, skip test since it would "
                        "take too long";
    }
    // PERF_VEHICLE_SPEED is a global property, so areaId is 0.
    if (!isVariableUpdateRateSupported(propConfig, /* areaId= */ 0)) {
        GTEST_SKIP() << "Variable update rate is not supported for PERF_VEHICLE_SPEED, "
                     << "skip testing";
    }

    // Subscribe to PERF_VEHICLE_SPEED using the max sample rate.
    auto client = mVhalClient->getSubscriptionClient(mCallback);
    ASSERT_NE(client, nullptr) << "Failed to get subscription client";
    SubscribeOptionsBuilder builder(propId);
    // By default variable update rate is true.
    builder.setSampleRate(maxSampleRate);
    auto option = builder.build();

    auto result = client->subscribe({option});

    ASSERT_TRUE(result.ok()) << StringPrintf("Failed to subscribe to property: %" PRId32
                                             ", error: %s",
                                             propId, result.error().message().c_str());

    // Sleep for 1 seconds to wait for more possible events to arrive.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    client->unsubscribe({propId});

    auto events = mCallback->getEvents(propId);
    if (events.size() <= 1) {
        // We received 0 or 1 event, the value is not changing so nothing to check here.
        // If all VHAL clients are subscribing to PERF_VEHICLE_SPEED with VUR on, then we
        // will receive 0 event. If there are other VHAL clients subscribing to PERF_VEHICLE_SPEED
        // with VUR off, then we will receive 1 event which is the initial value.
        return;
    }

    // Sort the values by the timestamp.
    std::map<int64_t, float> valuesByTimestamp;
    for (size_t i = 0; i < events.size(); i++) {
        valuesByTimestamp[events[i]->getTimestamp()] = events[i]->getFloatValues()[0];
    }

    size_t i = 0;
    float previousValue;
    for (const auto& [_, value] : valuesByTimestamp) {
        if (i != 0) {
            ASSERT_FALSE(value != previousValue) << "received duplicate value: " << value
                                                 << " when variable update rate is true";
        }
        previousValue = value;
        i++;
    }
}

// Test subscribe() with an invalid property.
TEST_P(VtsHalAutomotiveVehicleTargetTest, subscribeInvalidProp) {
    ALOGD("VtsHalAutomotiveVehicleTargetTest::subscribeInvalidProp");

    std::vector<SubscribeOptions> options = {
            SubscribeOptions{.propId = kInvalidProp, .sampleRate = 10.0}};

    auto client = mVhalClient->getSubscriptionClient(mCallback);
    ASSERT_NE(client, nullptr) << "Failed to get subscription client";

    auto result = client->subscribe(options);

    ASSERT_FALSE(result.ok()) << StringPrintf("Expect subscribing to property: %" PRId32 " to fail",
                                              kInvalidProp);
}

// Test the timestamp returned in GetValues results is the timestamp when the value is retrieved.
TEST_P(VtsHalAutomotiveVehicleTargetTest, testGetValuesTimestampAIDL) {
    if (!mVhalClient->isAidlVhal()) {
        GTEST_SKIP() << "Skip checking timestamp for HIDL because the behavior is only specified "
                        "for AIDL";
    }

    int32_t propId = toInt(VehicleProperty::PARKING_BRAKE_ON);
    if (!checkIsSupported(propId)) {
        GTEST_SKIP() << "Property: " << propId << " is not supported, skip the test";
    }
    auto prop = mVhalClient->createHalPropValue(propId);

    auto result = mVhalClient->getValueSync(*prop);

    ASSERT_TRUE(result.ok()) << StringPrintf("Failed to get value for property: %" PRId32
                                             ", error: %s",
                                             propId, result.error().message().c_str());
    ASSERT_NE(result.value(), nullptr) << "Result value must not be null";
    ASSERT_EQ(result.value()->getInt32Values().size(), 1u) << "Result must contain 1 int value";

    bool parkBrakeOnValue1 = (result.value()->getInt32Values()[0] == 1);
    int64_t timestampValue1 = result.value()->getTimestamp();

    result = mVhalClient->getValueSync(*prop);

    ASSERT_TRUE(result.ok()) << StringPrintf("Failed to get value for property: %" PRId32
                                             ", error: %s",
                                             propId, result.error().message().c_str());
    ASSERT_NE(result.value(), nullptr) << "Result value must not be null";
    ASSERT_EQ(result.value()->getInt32Values().size(), 1u) << "Result must contain 1 int value";

    bool parkBarkeOnValue2 = (result.value()->getInt32Values()[0] == 1);
    int64_t timestampValue2 = result.value()->getTimestamp();

    if (parkBarkeOnValue2 == parkBrakeOnValue1) {
        ASSERT_EQ(timestampValue2, timestampValue1)
                << "getValue result must contain a timestamp updated when the value was updated, if"
                   "the value does not change, expect the same timestamp";
    } else {
        ASSERT_GT(timestampValue2, timestampValue1)
                << "getValue result must contain a timestamp updated when the value was updated, if"
                   "the value changes, expect the newer value has a larger timestamp";
    }
}

TEST_P(VtsHalAutomotivePropertyConfigTest, verifyPropertyConfig) {
    const PropertyConfigTestParam& param = std::get<0>(GetParam());
    verifyProperty(param.propId, param.accessModes, param.changeMode);
}

std::vector<ServiceDescriptor> getDescriptors() {
    std::vector<ServiceDescriptor> descriptors;
    for (std::string name : getAidlHalInstanceNames(IVehicle::descriptor)) {
        descriptors.push_back({
                .name = name,
                .isAidlService = true,
        });
    }
    for (std::string name : getAllHalInstanceNames(
                 android::hardware::automotive::vehicle::V2_0::IVehicle::descriptor)) {
        descriptors.push_back({
                .name = name,
                .isAidlService = false,
        });
    }
    return descriptors;
}

std::vector<PropertyConfigTestParam> getPropertyConfigTestParams() {
    std::vector<PropertyConfigTestParam> testParams;
    for (const auto& [propId, accessModes] : AllowedAccessForVehicleProperty) {
        PropertyConfigTestParam param;
        param.propId = propId;
        param.accessModes = accessModes;
        param.changeMode = ChangeModeForVehicleProperty[propId];
        testParams.push_back(param);
    }
    return testParams;
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(VtsHalAutomotiveVehicleTargetTest);

INSTANTIATE_TEST_SUITE_P(PerInstance, VtsHalAutomotiveVehicleTargetTest,
                         testing::ValuesIn(getDescriptors()),
                         [](const testing::TestParamInfo<ServiceDescriptor>& info) {
                             std::string name = "";
                             if (info.param.isAidlService) {
                                 name += "aidl_";
                             } else {
                                 name += "hidl_";
                             }
                             name += info.param.name;
                             return Sanitize(name);
                         });

INSTANTIATE_TEST_SUITE_P(PerInstance, VtsHalAutomotivePropertyConfigTest,
                         testing::Combine(testing::ValuesIn(getPropertyConfigTestParams()),
                                          testing::ValuesIn(getDescriptors())),
                         [](const testing::TestParamInfo<
                                 std::tuple<PropertyConfigTestParam, ServiceDescriptor>>& info) {
                             std::string name = "";
                             if (std::get<1>(info.param).isAidlService) {
                                 name += "aidl_";
                             } else {
                                 name += "hidl_";
                             }
                             name += std::get<1>(info.param).name;
                             name += "_" + toString(std::get<0>(info.param).propId);
                             return Sanitize(name);
                         });

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    return RUN_ALL_TESTS();
}
