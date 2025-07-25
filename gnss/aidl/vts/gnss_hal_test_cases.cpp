/*
 * Copyright (C) 2020 The Android Open Source Project
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

#define LOG_TAG "GnssHalTestCases"

#include <android/hardware/gnss/IAGnss.h>
#include <android/hardware/gnss/IGnss.h>
#include <android/hardware/gnss/IGnssAntennaInfo.h>
#include <android/hardware/gnss/IGnssBatching.h>
#include <android/hardware/gnss/IGnssDebug.h>
#include <android/hardware/gnss/IGnssMeasurementCallback.h>
#include <android/hardware/gnss/IGnssMeasurementInterface.h>
#include <android/hardware/gnss/IGnssPowerIndication.h>
#include <android/hardware/gnss/IGnssPsds.h>
#include <android/hardware/gnss/gnss_assistance/IGnssAssistanceInterface.h>
#include <android/hardware/gnss/measurement_corrections/IMeasurementCorrectionsInterface.h>
#include <android/hardware/gnss/visibility_control/IGnssVisibilityControl.h>
#include <cutils/properties.h>
#include <utils/SystemClock.h>
#include <cmath>
#include <utility>
#include "AGnssCallbackAidl.h"
#include "AGnssRilCallbackAidl.h"
#include "GnssAntennaInfoCallbackAidl.h"
#include "GnssBatchingCallback.h"
#include "GnssGeofenceCallback.h"
#include "GnssMeasurementCallbackAidl.h"
#include "GnssNavigationMessageCallback.h"
#include "GnssPowerIndicationCallback.h"
#include "GnssVisibilityControlCallback.h"
#include "MeasurementCorrectionsCallback.h"
#include "Utils.h"
#include "gnss_hal_test.h"

using android::sp;
using android::hardware::gnss::BlocklistedSource;
using android::hardware::gnss::ElapsedRealtime;
using android::hardware::gnss::GnssClock;
using android::hardware::gnss::GnssConstellationType;
using android::hardware::gnss::GnssData;
using android::hardware::gnss::GnssLocation;
using android::hardware::gnss::GnssMeasurement;
using android::hardware::gnss::GnssPowerStats;
using android::hardware::gnss::IAGnss;
using android::hardware::gnss::IAGnssRil;
using android::hardware::gnss::IGnss;
using android::hardware::gnss::IGnssAntennaInfo;
using android::hardware::gnss::IGnssAntennaInfoCallback;
using android::hardware::gnss::IGnssBatching;
using android::hardware::gnss::IGnssBatchingCallback;
using android::hardware::gnss::IGnssCallback;
using android::hardware::gnss::IGnssConfiguration;
using android::hardware::gnss::IGnssDebug;
using android::hardware::gnss::IGnssGeofence;
using android::hardware::gnss::IGnssGeofenceCallback;
using android::hardware::gnss::IGnssMeasurementCallback;
using android::hardware::gnss::IGnssMeasurementInterface;
using android::hardware::gnss::IGnssNavigationMessageInterface;
using android::hardware::gnss::IGnssPowerIndication;
using android::hardware::gnss::IGnssPsds;
using android::hardware::gnss::PsdsType;
using android::hardware::gnss::SatellitePvt;
using android::hardware::gnss::common::Utils;
using android::hardware::gnss::gnss_assistance::AuxiliaryInformation;
using android::hardware::gnss::gnss_assistance::GnssAssistance;
using android::hardware::gnss::gnss_assistance::GpsSatelliteEphemeris;
using android::hardware::gnss::gnss_assistance::IGnssAssistanceInterface;
using android::hardware::gnss::measurement_corrections::IMeasurementCorrectionsInterface;
using android::hardware::gnss::visibility_control::IGnssVisibilityControl;

using GnssConstellationTypeV2_0 = android::hardware::gnss::V2_0::GnssConstellationType;
using GpsAssistance = android::hardware::gnss::gnss_assistance::GnssAssistance::GpsAssistance;

static bool IsAutomotiveDevice() {
    char buffer[PROPERTY_VALUE_MAX] = {0};
    property_get("ro.hardware.type", buffer, "");
    return strncmp(buffer, "automotive", PROPERTY_VALUE_MAX) == 0;
}

/*
 * SetupTeardownCreateCleanup:
 * Requests the gnss HAL then calls cleanup
 *
 * Empty test fixture to verify basic Setup & Teardown
 */
TEST_P(GnssHalTest, SetupTeardownCreateCleanup) {}

/*
 * GetLocation:
 * Turns on location, waits 75 second for at least 5 locations,
 * and checks them for reasonable validity.
 */
TEST_P(GnssHalTest, GetLocations) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    const int kMinIntervalMsec = 500;
    const int kLocationsToCheck = 5;

    SetPositionMode(kMinIntervalMsec, /* low_power_mode= */ false);
    StartAndCheckLocations(kLocationsToCheck);
    StopAndClearLocations();
}

/*
 * InjectDelete:
 * Ensures that calls to inject and/or delete information state are handled.
 */
TEST_P(GnssHalTest, InjectDelete) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    // Confidently, well north of Alaska
    auto status = aidl_gnss_hal_->injectLocation(Utils::getMockLocation(80.0, -170.0, 150.0));
    ASSERT_TRUE(status.isOk());

    // Fake time, but generally reasonable values (time in Aug. 2018)
    status =
            aidl_gnss_hal_->injectTime(/* timeMs= */ 1534567890123L,
                                       /* timeReferenceMs= */ 123456L, /* uncertaintyMs= */ 10000L);
    ASSERT_TRUE(status.isOk());

    status = aidl_gnss_hal_->deleteAidingData(IGnss::GnssAidingData::POSITION);
    ASSERT_TRUE(status.isOk());

    status = aidl_gnss_hal_->deleteAidingData(IGnss::GnssAidingData::TIME);
    ASSERT_TRUE(status.isOk());

    // Ensure we can get a good location after a bad injection has been deleted
    StartAndCheckFirstLocation(/* min_interval_msec= */ 1000, /* low_power_mode= */ false);
    StopAndClearLocations();
}

/*
 * InjectSeedLocation:
 * Injects a seed location and ensures the injected seed location is not fused in the resulting
 * GNSS location.
 */
TEST_P(GnssHalTest, InjectSeedLocation) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    // An arbitrary position in North Pacific Ocean (where no VTS labs will ever likely be located).
    const double seedLatDegrees = 32.312894;
    const double seedLngDegrees = -172.954117;
    const float seedAccuracyMeters = 150.0;

    auto status = aidl_gnss_hal_->injectLocation(
            Utils::getMockLocation(seedLatDegrees, seedLngDegrees, seedAccuracyMeters));
    ASSERT_TRUE(status.isOk());

    StartAndCheckFirstLocation(/* min_interval_msec= */ 1000, /* low_power_mode= */ false);

    // Ensure we don't get a location anywhere within 111km (1 degree of lat or lng) of the seed
    // location.
    EXPECT_TRUE(std::abs(aidl_gnss_cb_->last_location_.latitudeDegrees - seedLatDegrees) > 1.0 ||
                std::abs(aidl_gnss_cb_->last_location_.longitudeDegrees - seedLngDegrees) > 1.0);

    StopAndClearLocations();

    status = aidl_gnss_hal_->deleteAidingData(IGnss::GnssAidingData::POSITION);
    ASSERT_TRUE(status.isOk());
}

/*
 * GnssCapabilities:
 * 1. Verifies that GNSS hardware supports measurement capabilities.
 * 2. Verifies that GNSS hardware supports Scheduling capabilities.
 * 3. Verifies that GNSS hardware supports non-empty signal type capabilities.
 */
TEST_P(GnssHalTest, GnssCapabilites) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    if (!IsAutomotiveDevice()) {
        EXPECT_TRUE(aidl_gnss_cb_->last_capabilities_ & IGnssCallback::CAPABILITY_MEASUREMENTS);
    }
    EXPECT_TRUE(aidl_gnss_cb_->last_capabilities_ & IGnssCallback::CAPABILITY_SCHEDULING);
    if (aidl_gnss_hal_->getInterfaceVersion() <= 2) {
        return;
    }
    EXPECT_FALSE(aidl_gnss_cb_->last_signal_type_capabilities.empty());
}

/*
 * GetLocationLowPower:
 * Turns on location, waits for at least 5 locations allowing max of LOCATION_TIMEOUT_SUBSEQUENT_SEC
 * between one location and the next. Also ensure that MIN_INTERVAL_MSEC is respected by waiting
 * NO_LOCATION_PERIOD_SEC and verfiy that no location is received. Also perform validity checks on
 * each received location.
 */
TEST_P(GnssHalTest, GetLocationLowPower) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }

    const int kMinIntervalMsec = 5000;
    const int kLocationTimeoutSubsequentSec = (kMinIntervalMsec / 1000) * 2;
    const int kNoLocationPeriodSec = (kMinIntervalMsec / 1000) / 2;
    const int kLocationsToCheck = 5;
    const bool kLowPowerMode = true;

    // Warmup period - VTS doesn't have AGPS access via GnssLocationProvider
    aidl_gnss_cb_->location_cbq_.reset();
    StartAndCheckLocations(kLocationsToCheck);
    StopAndClearLocations();
    aidl_gnss_cb_->location_cbq_.reset();

    // Start of Low Power Mode test
    // Don't expect true - as without AGPS access
    if (!StartAndCheckFirstLocation(kMinIntervalMsec, kLowPowerMode)) {
        ALOGW("GetLocationLowPower test - no first low power location received.");
    }

    for (int i = 1; i < kLocationsToCheck; i++) {
        // Verify that kMinIntervalMsec is respected by waiting kNoLocationPeriodSec and
        // ensure that no location is received yet

        aidl_gnss_cb_->location_cbq_.retrieve(aidl_gnss_cb_->last_location_, kNoLocationPeriodSec);
        const int location_called_count = aidl_gnss_cb_->location_cbq_.calledCount();
        // Tolerate (ignore) one extra location right after the first one
        // to handle startup edge case scheduling limitations in some implementations
        if ((i == 1) && (location_called_count == 2)) {
            CheckLocation(aidl_gnss_cb_->last_location_, true);
            continue;  // restart the quiet wait period after this too-fast location
        }
        EXPECT_LE(location_called_count, i);
        if (location_called_count != i) {
            ALOGW("GetLocationLowPower test - not enough locations received. %d vs. %d expected ",
                  location_called_count, i);
        }

        if (!aidl_gnss_cb_->location_cbq_.retrieve(
                    aidl_gnss_cb_->last_location_,
                    kLocationTimeoutSubsequentSec - kNoLocationPeriodSec)) {
            ALOGW("GetLocationLowPower test - timeout awaiting location %d", i);
        } else {
            CheckLocation(aidl_gnss_cb_->last_location_, true);
        }
    }

    StopAndClearLocations();
}

/*
 * InjectBestLocation
 *
 * Ensure successfully injecting a location.
 */
TEST_P(GnssHalTest, InjectBestLocation) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    StartAndCheckLocations(1);
    GnssLocation gnssLocation = aidl_gnss_cb_->last_location_;
    CheckLocation(gnssLocation, true);

    auto status = aidl_gnss_hal_->injectBestLocation(gnssLocation);

    ASSERT_TRUE(status.isOk());

    status = aidl_gnss_hal_->deleteAidingData(IGnss::GnssAidingData::POSITION);

    ASSERT_TRUE(status.isOk());
}

/*
 * TestGnssSvInfoFields:
 * Gets 1 location and a (non-empty) GnssSvInfo, and verifies basebandCN0DbHz is valid.
 */
TEST_P(GnssHalTest, TestGnssSvInfoFields) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    aidl_gnss_cb_->location_cbq_.reset();
    aidl_gnss_cb_->sv_info_list_cbq_.reset();
    StartAndCheckFirstLocation(/* min_interval_msec= */ 1000, /* low_power_mode= */ false);
    int location_called_count = aidl_gnss_cb_->location_cbq_.calledCount();
    ALOGD("Observed %d GnssSvStatus, while awaiting one location (%d received)",
          aidl_gnss_cb_->sv_info_list_cbq_.size(), location_called_count);

    // Wait for up to kNumSvInfoLists events for kTimeoutSeconds for each event.
    int kTimeoutSeconds = 2;
    int kNumSvInfoLists = 4;
    std::list<std::vector<IGnssCallback::GnssSvInfo>> sv_info_lists;
    std::vector<IGnssCallback::GnssSvInfo> last_sv_info_list;

    do {
        EXPECT_GT(aidl_gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_lists, kNumSvInfoLists,
                                                            kTimeoutSeconds),
                  0);
        if (!sv_info_lists.empty()) {
            last_sv_info_list = sv_info_lists.back();
            ALOGD("last_sv_info size = %d", (int)last_sv_info_list.size());
        }
    } while (!sv_info_lists.empty() && last_sv_info_list.size() == 0);

    bool nonZeroCn0Found = false;
    for (auto sv_info : last_sv_info_list) {
        EXPECT_TRUE(sv_info.basebandCN0DbHz >= 0.0 && sv_info.basebandCN0DbHz <= 65.0);
        if (sv_info.basebandCN0DbHz > 0.0) {
            nonZeroCn0Found = true;
        }
    }
    // Assert at least one value is non-zero. Zero is ok in status as it's possibly
    // reporting a searched but not found satellite.
    EXPECT_TRUE(nonZeroCn0Found);
    StopAndClearLocations();
}

/*
 * TestPsdsExtension:
 * 1. Gets the PsdsExtension
 * 2. Injects empty PSDS data and verifies that it returns an error.
 */
TEST_P(GnssHalTest, TestPsdsExtension) {
    sp<IGnssPsds> iGnssPsds;
    auto status = aidl_gnss_hal_->getExtensionPsds(&iGnssPsds);
    if (status.isOk() && iGnssPsds != nullptr) {
        status = iGnssPsds->injectPsdsData(PsdsType::LONG_TERM, std::vector<uint8_t>());
        ASSERT_FALSE(status.isOk());
    }
}

void CheckSatellitePvt(const SatellitePvt& satellitePvt, const int interfaceVersion) {
    const double kMaxOrbitRadiusMeters = 43000000.0;
    const double kMaxVelocityMps = 4000.0;
    // The below values are determined using GPS ICD Table 20-1
    const double kMinHardwareCodeBiasMeters = -17.869;
    const double kMaxHardwareCodeBiasMeters = 17.729;
    const double kMaxTimeCorrelationMeters = 3e6;
    const double kMaxSatClkDriftMps = 1.117;

    ASSERT_TRUE(satellitePvt.flags & SatellitePvt::HAS_POSITION_VELOCITY_CLOCK_INFO ||
                satellitePvt.flags & SatellitePvt::HAS_IONO ||
                satellitePvt.flags & SatellitePvt::HAS_TROPO);
    if (satellitePvt.flags & SatellitePvt::HAS_POSITION_VELOCITY_CLOCK_INFO) {
        ALOGD("Found HAS_POSITION_VELOCITY_CLOCK_INFO");
        ASSERT_TRUE(satellitePvt.satPosEcef.posXMeters >= -kMaxOrbitRadiusMeters &&
                    satellitePvt.satPosEcef.posXMeters <= kMaxOrbitRadiusMeters);
        ASSERT_TRUE(satellitePvt.satPosEcef.posYMeters >= -kMaxOrbitRadiusMeters &&
                    satellitePvt.satPosEcef.posYMeters <= kMaxOrbitRadiusMeters);
        ASSERT_TRUE(satellitePvt.satPosEcef.posZMeters >= -kMaxOrbitRadiusMeters &&
                    satellitePvt.satPosEcef.posZMeters <= kMaxOrbitRadiusMeters);
        ASSERT_TRUE(satellitePvt.satPosEcef.ureMeters > 0);
        ASSERT_TRUE(satellitePvt.satVelEcef.velXMps >= -kMaxVelocityMps &&
                    satellitePvt.satVelEcef.velXMps <= kMaxVelocityMps);
        ASSERT_TRUE(satellitePvt.satVelEcef.velYMps >= -kMaxVelocityMps &&
                    satellitePvt.satVelEcef.velYMps <= kMaxVelocityMps);
        ASSERT_TRUE(satellitePvt.satVelEcef.velZMps >= -kMaxVelocityMps &&
                    satellitePvt.satVelEcef.velZMps <= kMaxVelocityMps);
        ASSERT_TRUE(satellitePvt.satVelEcef.ureRateMps > 0);
        ASSERT_TRUE(
                satellitePvt.satClockInfo.satHardwareCodeBiasMeters > kMinHardwareCodeBiasMeters &&
                satellitePvt.satClockInfo.satHardwareCodeBiasMeters < kMaxHardwareCodeBiasMeters);
        ASSERT_TRUE(satellitePvt.satClockInfo.satTimeCorrectionMeters >
                            -kMaxTimeCorrelationMeters &&
                    satellitePvt.satClockInfo.satTimeCorrectionMeters < kMaxTimeCorrelationMeters);
        ASSERT_TRUE(satellitePvt.satClockInfo.satClkDriftMps > -kMaxSatClkDriftMps &&
                    satellitePvt.satClockInfo.satClkDriftMps < kMaxSatClkDriftMps);
    }
    if (satellitePvt.flags & SatellitePvt::HAS_IONO) {
        ALOGD("Found HAS_IONO");
        ASSERT_TRUE(satellitePvt.ionoDelayMeters > 0 && satellitePvt.ionoDelayMeters < 100);
    }
    if (satellitePvt.flags & SatellitePvt::HAS_TROPO) {
        ALOGD("Found HAS_TROPO");
        ASSERT_TRUE(satellitePvt.tropoDelayMeters > 0 && satellitePvt.tropoDelayMeters < 100);
    }
    if (interfaceVersion >= 2) {
        ASSERT_TRUE(satellitePvt.timeOfClockSeconds >= 0);
        ASSERT_TRUE(satellitePvt.timeOfEphemerisSeconds >= 0);
        // IODC has 10 bits
        ASSERT_TRUE(satellitePvt.issueOfDataClock >= 0 && satellitePvt.issueOfDataClock <= 1023);
        // IODE has 8 bits
        ASSERT_TRUE(satellitePvt.issueOfDataEphemeris >= 0 &&
                    satellitePvt.issueOfDataEphemeris <= 255);
    }
}

/*
 * TestGnssMeasurementExtensionAndSatellitePvt:
 * 1. Gets the GnssMeasurementExtension and verifies that it returns a non-null extension.
 * 2. Sets a GnssMeasurementCallback, waits for a measurement, and verifies mandatory fields are
 *    valid.
 * 3. If SatellitePvt is supported, waits for a measurement with SatellitePvt, and verifies the
 *    fields are valid.
 */
TEST_P(GnssHalTest, TestGnssMeasurementExtensionAndSatellitePvt) {
    const bool kIsSatellitePvtSupported =
            aidl_gnss_cb_->last_capabilities_ & (int)GnssCallbackAidl::CAPABILITY_SATELLITE_PVT;
    ALOGD("SatellitePvt supported: %s", kIsSatellitePvtSupported ? "true" : "false");
    const int kFirstGnssMeasurementTimeoutSeconds = 10;
    const int kNumMeasurementEvents = 75;

    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    auto callback = sp<GnssMeasurementCallbackAidl>::make();
    status = iGnssMeasurement->setCallback(callback, /* enableFullTracking= */ true,
                                           /* enableCorrVecOutputs */ false);
    ASSERT_TRUE(status.isOk());

    bool satellitePvtFound = false;
    for (int i = 0; i < kNumMeasurementEvents; i++) {
        if (i > 0 && (!kIsSatellitePvtSupported || satellitePvtFound)) {
            break;
        }
        GnssData lastMeasurement;
        ASSERT_TRUE(callback->gnss_data_cbq_.retrieve(lastMeasurement,
                                                      kFirstGnssMeasurementTimeoutSeconds));
        EXPECT_EQ(callback->gnss_data_cbq_.calledCount(), i + 1);
        if (i <= 2 && lastMeasurement.measurements.size() == 0) {
            // Allow 3 seconds tolerance for empty measurement
            continue;
        }
        ASSERT_TRUE(lastMeasurement.measurements.size() > 0);

        // Validity check GnssData fields
        checkGnssMeasurementClockFields(lastMeasurement);

        for (const auto& measurement : lastMeasurement.measurements) {
            checkGnssMeasurementFields(measurement, lastMeasurement);
            if (measurement.flags & GnssMeasurement::HAS_SATELLITE_PVT &&
                kIsSatellitePvtSupported == true) {
                ALOGD("Found a measurement with SatellitePvt");
                satellitePvtFound = true;
                CheckSatellitePvt(measurement.satellitePvt, aidl_gnss_hal_->getInterfaceVersion());
            }
        }
    }
    if (kIsSatellitePvtSupported) {
        ASSERT_TRUE(satellitePvtFound);
    }

    status = iGnssMeasurement->close();
    ASSERT_TRUE(status.isOk());
}

/*
 * TestCorrelationVector:
 * 1. Gets the GnssMeasurementExtension and verifies that it returns a non-null extension.
 * 2. Sets a GnssMeasurementCallback, waits for GnssMeasurements with CorrelationVector, and
 *    verifies fields are valid.
 */
TEST_P(GnssHalTest, TestCorrelationVector) {
    const bool kIsCorrelationVectorSupported = aidl_gnss_cb_->last_capabilities_ &
                                               (int)GnssCallbackAidl::CAPABILITY_CORRELATION_VECTOR;
    const int kNumMeasurementEvents = 75;
    // Pass the test if CorrelationVector is not supported
    if (!kIsCorrelationVectorSupported) {
        return;
    }

    const int kFirstGnssMeasurementTimeoutSeconds = 10;
    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    auto callback = sp<GnssMeasurementCallbackAidl>::make();
    status =
            iGnssMeasurement->setCallback(callback, /* enableFullTracking= */ true,
                                          /* enableCorrVecOutputs */ kIsCorrelationVectorSupported);
    ASSERT_TRUE(status.isOk());

    bool correlationVectorFound = false;
    for (int i = 0; i < kNumMeasurementEvents; i++) {
        // Pass the test if at least one CorrelationVector has been found.
        if (correlationVectorFound) {
            break;
        }
        GnssData lastMeasurement;
        ASSERT_TRUE(callback->gnss_data_cbq_.retrieve(lastMeasurement,
                                                      kFirstGnssMeasurementTimeoutSeconds));
        EXPECT_EQ(callback->gnss_data_cbq_.calledCount(), i + 1);
        if (i <= 2 && lastMeasurement.measurements.size() == 0) {
            // Allow 3 seconds tolerance for empty measurement
            continue;
        }
        ASSERT_TRUE(lastMeasurement.measurements.size() > 0);

        // Validity check GnssData fields
        checkGnssMeasurementClockFields(lastMeasurement);

        for (const auto& measurement : lastMeasurement.measurements) {
            checkGnssMeasurementFields(measurement, lastMeasurement);
            if (measurement.flags & GnssMeasurement::HAS_CORRELATION_VECTOR) {
                correlationVectorFound = true;
                ASSERT_TRUE(measurement.correlationVectors.size() > 0);
                for (const auto& correlationVector : measurement.correlationVectors) {
                    ASSERT_GE(correlationVector.frequencyOffsetMps, 0);
                    ASSERT_GT(correlationVector.samplingWidthM, 0);
                    ASSERT_TRUE(correlationVector.magnitude.size() > 0);
                    for (const auto& magnitude : correlationVector.magnitude) {
                        ASSERT_TRUE(magnitude >= -32768 && magnitude <= 32767);
                    }
                }
            }
        }
    }
    ASSERT_TRUE(correlationVectorFound);

    status = iGnssMeasurement->close();
    ASSERT_TRUE(status.isOk());
}

/*
 * TestGnssPowerIndication
 * 1. Gets the GnssPowerIndicationExtension.
 * 2. Sets a GnssPowerIndicationCallback.
 * 3. Requests and verifies the 1st GnssPowerStats is received.
 * 4. Gets a location.
 * 5. Requests the 2nd GnssPowerStats, and verifies it has larger values than the 1st one.
 */
TEST_P(GnssHalTest, TestGnssPowerIndication) {
    // Set up gnssPowerIndication and callback
    sp<IGnssPowerIndication> iGnssPowerIndication;
    auto status = aidl_gnss_hal_->getExtensionGnssPowerIndication(&iGnssPowerIndication);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssPowerIndication != nullptr);

    auto gnssPowerIndicationCallback = sp<GnssPowerIndicationCallback>::make();
    status = iGnssPowerIndication->setCallback(gnssPowerIndicationCallback);
    ASSERT_TRUE(status.isOk());

    const int kTimeoutSec = 2;
    EXPECT_TRUE(gnssPowerIndicationCallback->capabilities_cbq_.retrieve(
            gnssPowerIndicationCallback->last_capabilities_, kTimeoutSec));

    EXPECT_EQ(gnssPowerIndicationCallback->capabilities_cbq_.calledCount(), 1);

    if (gnssPowerIndicationCallback->last_capabilities_ == 0) {
        // Skipping the test since GnssPowerIndication is not supported.
        return;
    }

    // Request and verify a GnssPowerStats is received
    gnssPowerIndicationCallback->gnss_power_stats_cbq_.reset();
    iGnssPowerIndication->requestGnssPowerStats();

    EXPECT_TRUE(gnssPowerIndicationCallback->gnss_power_stats_cbq_.retrieve(
            gnssPowerIndicationCallback->last_gnss_power_stats_, kTimeoutSec));
    EXPECT_EQ(gnssPowerIndicationCallback->gnss_power_stats_cbq_.calledCount(), 1);
    auto powerStats1 = gnssPowerIndicationCallback->last_gnss_power_stats_;

    // Get a location and request another GnssPowerStats
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        gnss_cb_->location_cbq_.reset();
    } else {
        aidl_gnss_cb_->location_cbq_.reset();
    }
    StartAndCheckFirstLocation(/* min_interval_msec= */ 1000, /* low_power_mode= */ false);

    // Request and verify the 2nd GnssPowerStats has larger values than the 1st one
    iGnssPowerIndication->requestGnssPowerStats();

    EXPECT_TRUE(gnssPowerIndicationCallback->gnss_power_stats_cbq_.retrieve(
            gnssPowerIndicationCallback->last_gnss_power_stats_, kTimeoutSec));
    EXPECT_EQ(gnssPowerIndicationCallback->gnss_power_stats_cbq_.calledCount(), 2);

    auto powerStats2 = gnssPowerIndicationCallback->last_gnss_power_stats_;

    if ((gnssPowerIndicationCallback->last_capabilities_ &
         (int)GnssPowerIndicationCallback::CAPABILITY_TOTAL)) {
        // Elapsed realtime must increase
        EXPECT_GT(powerStats2.elapsedRealtime.timestampNs, powerStats1.elapsedRealtime.timestampNs);

        // Total energy must increase
        EXPECT_GT(powerStats2.totalEnergyMilliJoule, powerStats1.totalEnergyMilliJoule);
    }

    // At least oone of singleband and multiband acquisition energy must increase
    bool singlebandAcqEnergyIncreased = powerStats2.singlebandAcquisitionModeEnergyMilliJoule >
                                        powerStats1.singlebandAcquisitionModeEnergyMilliJoule;
    bool multibandAcqEnergyIncreased = powerStats2.multibandAcquisitionModeEnergyMilliJoule >
                                       powerStats1.multibandAcquisitionModeEnergyMilliJoule;

    if ((gnssPowerIndicationCallback->last_capabilities_ &
         (int)GnssPowerIndicationCallback::CAPABILITY_SINGLEBAND_ACQUISITION) ||
        (gnssPowerIndicationCallback->last_capabilities_ &
         (int)GnssPowerIndicationCallback::CAPABILITY_MULTIBAND_ACQUISITION)) {
        EXPECT_TRUE(singlebandAcqEnergyIncreased || multibandAcqEnergyIncreased);
    }

    // At least one of singleband and multiband tracking energy must increase
    bool singlebandTrackingEnergyIncreased = powerStats2.singlebandTrackingModeEnergyMilliJoule >
                                             powerStats1.singlebandTrackingModeEnergyMilliJoule;
    bool multibandTrackingEnergyIncreased = powerStats2.multibandTrackingModeEnergyMilliJoule >
                                            powerStats1.multibandTrackingModeEnergyMilliJoule;
    if ((gnssPowerIndicationCallback->last_capabilities_ &
         (int)GnssPowerIndicationCallback::CAPABILITY_SINGLEBAND_TRACKING) ||
        (gnssPowerIndicationCallback->last_capabilities_ &
         (int)GnssPowerIndicationCallback::CAPABILITY_MULTIBAND_TRACKING)) {
        EXPECT_TRUE(singlebandTrackingEnergyIncreased || multibandTrackingEnergyIncreased);
    }

    // Clean up
    StopAndClearLocations();
}

/*
 * BlocklistIndividualSatellites:
 *
 * 1) Turns on location, waits for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus for common satellites (strongest one in each constellation.)
 * 2a & b) Turns off location, and blocklists common satellites.
 * 3) Restart location, wait for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus does not use those satellites.
 * 4a & b) Turns off location, and send in empty blocklist.
 * 5a) Restart location, wait for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus does re-use at least the previously strongest satellite
 * 5b) Retry a few times, in case GNSS search strategy takes a while to reacquire even the
 * formerly strongest satellite
 */
TEST_P(GnssHalTest, BlocklistIndividualSatellites) {
    if (!(aidl_gnss_cb_->last_capabilities_ &
          (int)GnssCallbackAidl::CAPABILITY_SATELLITE_BLOCKLIST)) {
        ALOGI("Test BlocklistIndividualSatellites skipped. SATELLITE_BLOCKLIST capability not "
              "supported.");
        return;
    }

    const int kWarmUpLocations = 3;
    const int kLocationsToAwait = 3;
    const int kRetriesToUnBlocklist = 10;

    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        gnss_cb_->location_cbq_.reset();
    } else {
        aidl_gnss_cb_->location_cbq_.reset();
    }
    StartAndCheckLocations(kLocationsToAwait + kWarmUpLocations);
    int location_called_count = (aidl_gnss_hal_->getInterfaceVersion() <= 1)
                                        ? gnss_cb_->location_cbq_.calledCount()
                                        : aidl_gnss_cb_->location_cbq_.calledCount();

    // Tolerate 1 less sv status to handle edge cases in reporting.
    int sv_info_list_cbq_size = (aidl_gnss_hal_->getInterfaceVersion() <= 1)
                                        ? gnss_cb_->sv_info_list_cbq_.size()
                                        : aidl_gnss_cb_->sv_info_list_cbq_.size();
    EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait + kWarmUpLocations);
    ALOGD("Observed %d GnssSvInfo, while awaiting %d Locations (%d received)",
          sv_info_list_cbq_size, kLocationsToAwait + kWarmUpLocations, location_called_count);

    /*
     * Identify strongest SV per constellation seen seen at least kLocationsToAwait -1 times.
     *
     * Why not (kLocationsToAwait + kWarmUpLocations)?  To avoid test flakiness in case of
     * (plausible) slight flakiness in strongest signal observability (one epoch RF null)
     */

    const int kGnssSvInfoListTimeout = 2;
    std::vector<BlocklistedSource> sources_to_blocklist;
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        // Discard kWarmUpLocations sv_info_vec
        std::list<hidl_vec<IGnssCallback_2_1::GnssSvInfo>> tmp;
        int count =
                gnss_cb_->sv_info_list_cbq_.retrieve(tmp, kWarmUpLocations, kGnssSvInfoListTimeout);
        ASSERT_EQ(count, kWarmUpLocations);

        // Retrieve (sv_info_list_cbq_size - kWarmUpLocations) sv_info_vec
        std::list<hidl_vec<IGnssCallback_2_1::GnssSvInfo>> sv_info_vec_list;
        count = gnss_cb_->sv_info_list_cbq_.retrieve(
                sv_info_vec_list, sv_info_list_cbq_size - kWarmUpLocations, kGnssSvInfoListTimeout);
        ASSERT_EQ(count, sv_info_list_cbq_size - kWarmUpLocations);
        sources_to_blocklist = FindStrongFrequentSources(sv_info_vec_list, kLocationsToAwait - 1);
    } else {
        // Discard kWarmUpLocations sv_info_vec
        std::list<std::vector<IGnssCallback::GnssSvInfo>> tmp;
        int count = aidl_gnss_cb_->sv_info_list_cbq_.retrieve(tmp, kWarmUpLocations,
                                                              kGnssSvInfoListTimeout);
        ASSERT_EQ(count, kWarmUpLocations);

        // Retrieve (sv_info_list_cbq_size - kWarmUpLocations) sv_info_vec
        std::list<std::vector<IGnssCallback::GnssSvInfo>> sv_info_vec_list;
        count = aidl_gnss_cb_->sv_info_list_cbq_.retrieve(
                sv_info_vec_list, sv_info_list_cbq_size - kWarmUpLocations, kGnssSvInfoListTimeout);
        ASSERT_EQ(count, sv_info_list_cbq_size - kWarmUpLocations);
        sources_to_blocklist = FindStrongFrequentSources(sv_info_vec_list, kLocationsToAwait - 1);
    }

    if (sources_to_blocklist.empty()) {
        // Cannot find a satellite to blocklist. Let the test pass.
        ALOGD("Cannot find a satellite to blocklist. Letting the test pass.");
        return;
    }

    // Stop locations, blocklist the common SV
    StopAndClearLocations();

    sp<IGnssConfiguration> gnss_configuration_hal;
    auto status = aidl_gnss_hal_->getExtensionGnssConfiguration(&gnss_configuration_hal);
    ASSERT_TRUE(status.isOk());
    ASSERT_NE(gnss_configuration_hal, nullptr);

    std::vector<BlocklistedSource> sources;
    sources = sources_to_blocklist;
    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());

    // retry and ensure satellite not used
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        gnss_cb_->sv_info_list_cbq_.reset();
        gnss_cb_->location_cbq_.reset();
    } else {
        aidl_gnss_cb_->sv_info_list_cbq_.reset();
        aidl_gnss_cb_->location_cbq_.reset();
    }

    StartAndCheckLocations(kLocationsToAwait);

    // early exit if test is being run with insufficient signal
    location_called_count = (aidl_gnss_hal_->getInterfaceVersion() <= 1)
                                    ? gnss_cb_->location_cbq_.calledCount()
                                    : aidl_gnss_cb_->location_cbq_.calledCount();
    if (location_called_count == 0) {
        ALOGE("0 Gnss locations received - ensure sufficient signal and retry");
    }
    ASSERT_TRUE(location_called_count > 0);

    // Tolerate 1 less sv status to handle edge cases in reporting.
    sv_info_list_cbq_size = (aidl_gnss_hal_->getInterfaceVersion() <= 1)
                                    ? gnss_cb_->sv_info_list_cbq_.size()
                                    : aidl_gnss_cb_->sv_info_list_cbq_.size();
    EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
    ALOGD("Observed %d GnssSvInfo, while awaiting %d Locations (%d received)",
          sv_info_list_cbq_size, kLocationsToAwait, location_called_count);
    bool isCnBuild = Utils::isCnBuild();
    for (int i = 0; i < sv_info_list_cbq_size; ++i) {
        if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
            hidl_vec<IGnssCallback_2_1::GnssSvInfo> sv_info_vec;
            gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
            for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                auto& gnss_sv = sv_info_vec[iSv];
                if (!(gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX)) {
                    continue;
                }
                for (auto const& source : sources_to_blocklist) {
                    if (isBlockableConstellation(source.constellation, isCnBuild)) {
                        EXPECT_FALSE((gnss_sv.v2_0.v1_0.svid == source.svid) &&
                                     (static_cast<GnssConstellationType>(
                                              gnss_sv.v2_0.constellation) == source.constellation));
                    } else if ((gnss_sv.v2_0.v1_0.svid == source.svid) &&
                               (static_cast<GnssConstellationType>(gnss_sv.v2_0.constellation) ==
                                source.constellation)) {
                        ALOGW("Found constellation %d, svid %d blocklisted but still used-in-fix.",
                              source.constellation, source.svid);
                    }
                }
            }
        } else {
            std::vector<IGnssCallback::GnssSvInfo> sv_info_vec;
            aidl_gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
            for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                auto& gnss_sv = sv_info_vec[iSv];
                if (!(gnss_sv.svFlag & (int)IGnssCallback::GnssSvFlags::USED_IN_FIX)) {
                    continue;
                }
                for (auto const& source : sources_to_blocklist) {
                    if (isBlockableConstellation(source.constellation, isCnBuild)) {
                        EXPECT_FALSE((gnss_sv.svid == source.svid) &&
                                     (gnss_sv.constellation == source.constellation));
                    } else if ((gnss_sv.svid == source.svid) &&
                               (gnss_sv.constellation == source.constellation)) {
                        ALOGW("Found constellation %d, svid %d blocklisted but still used-in-fix.",
                              gnss_sv.constellation, gnss_sv.svid);
                    }
                }
            }
        }
    }

    // clear blocklist and restart - this time updating the blocklist while location is still on
    sources.resize(0);

    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());

    bool strongest_sv_is_reobserved = false;
    // do several loops awaiting a few locations, allowing non-immediate reacquisition strategies
    int unblocklist_loops_remaining = kRetriesToUnBlocklist;
    while (!strongest_sv_is_reobserved && (unblocklist_loops_remaining-- > 0)) {
        StopAndClearLocations();

        if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
            gnss_cb_->sv_info_list_cbq_.reset();
            gnss_cb_->location_cbq_.reset();
        } else {
            aidl_gnss_cb_->sv_info_list_cbq_.reset();
            aidl_gnss_cb_->location_cbq_.reset();
        }
        StartAndCheckLocations(kLocationsToAwait);

        // early exit loop if test is being run with insufficient signal
        location_called_count = (aidl_gnss_hal_->getInterfaceVersion() <= 1)
                                        ? gnss_cb_->location_cbq_.calledCount()
                                        : aidl_gnss_cb_->location_cbq_.calledCount();
        if (location_called_count == 0) {
            ALOGE("0 Gnss locations received - ensure sufficient signal and retry");
        }
        ASSERT_TRUE(location_called_count > 0);

        // Tolerate 1 less sv status to handle edge cases in reporting.
        sv_info_list_cbq_size = (aidl_gnss_hal_->getInterfaceVersion() <= 1)
                                        ? gnss_cb_->sv_info_list_cbq_.size()
                                        : aidl_gnss_cb_->sv_info_list_cbq_.size();
        EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
        ALOGD("Clear blocklist, observed %d GnssSvInfo, while awaiting %d Locations"
              ", tries remaining %d",
              sv_info_list_cbq_size, kLocationsToAwait, unblocklist_loops_remaining);

        for (int i = 0; i < sv_info_list_cbq_size; ++i) {
            if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
                hidl_vec<IGnssCallback_2_1::GnssSvInfo> sv_info_vec;
                gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
                for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                    auto& gnss_sv = sv_info_vec[iSv];
                    for (auto const& source : sources_to_blocklist) {
                        if ((gnss_sv.v2_0.v1_0.svid == source.svid) &&
                            (static_cast<GnssConstellationType>(gnss_sv.v2_0.constellation) ==
                             source.constellation) &&
                            (gnss_sv.v2_0.v1_0.svFlag &
                             IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX)) {
                            strongest_sv_is_reobserved = true;
                            break;
                        }
                    }
                }
            } else {
                std::vector<IGnssCallback::GnssSvInfo> sv_info_vec;
                aidl_gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
                for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                    auto& gnss_sv = sv_info_vec[iSv];
                    for (auto const& source : sources_to_blocklist) {
                        if ((gnss_sv.svid == source.svid) &&
                            (gnss_sv.constellation == source.constellation) &&
                            (gnss_sv.svFlag & (int)IGnssCallback::GnssSvFlags::USED_IN_FIX)) {
                            strongest_sv_is_reobserved = true;
                            break;
                        }
                    }
                }
            }
            if (strongest_sv_is_reobserved) break;
        }
    }
    EXPECT_TRUE(strongest_sv_is_reobserved);
    StopAndClearLocations();
}

/*
 * BlocklistConstellationLocationOff:
 *
 * 1) Turns on location, waits for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus for any blockable constellations.
 * 2a & b) Turns off location, and blocklist first blockable constellations.
 * 3) Restart location, wait for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus does not use any constellation but GPS.
 * 4a & b) Clean up by turning off location, and send in empty blocklist.
 */
TEST_P(GnssHalTest, BlocklistConstellationLocationOff) {
    if (!(aidl_gnss_cb_->last_capabilities_ &
          (int)GnssCallbackAidl::CAPABILITY_SATELLITE_BLOCKLIST)) {
        ALOGI("Test BlocklistConstellationLocationOff skipped. SATELLITE_BLOCKLIST capability not "
              "supported.");
        return;
    }

    const int kLocationsToAwait = 3;
    const int kGnssSvInfoListTimeout = 2;

    // Find first blockable constellation to blocklist
    GnssConstellationType constellation_to_blocklist = static_cast<GnssConstellationType>(
            startLocationAndGetBlockableConstellation(kLocationsToAwait, kGnssSvInfoListTimeout));

    // Turns off location
    StopAndClearLocations();

    BlocklistedSource source_to_blocklist_1;
    source_to_blocklist_1.constellation = constellation_to_blocklist;
    source_to_blocklist_1.svid = 0;  // documented wildcard for all satellites in this constellation

    // IRNSS was added in 2.0. Always attempt to blocklist IRNSS to verify that the new enum is
    // supported.
    BlocklistedSource source_to_blocklist_2;
    source_to_blocklist_2.constellation = GnssConstellationType::IRNSS;
    source_to_blocklist_2.svid = 0;  // documented wildcard for all satellites in this constellation

    sp<IGnssConfiguration> gnss_configuration_hal;
    auto status = aidl_gnss_hal_->getExtensionGnssConfiguration(&gnss_configuration_hal);
    ASSERT_TRUE(status.isOk());
    ASSERT_NE(gnss_configuration_hal, nullptr);

    hidl_vec<BlocklistedSource> sources;
    sources.resize(2);
    sources[0] = source_to_blocklist_1;
    sources[1] = source_to_blocklist_2;

    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());

    // retry and ensure constellation not used
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        gnss_cb_->sv_info_list_cbq_.reset();
        gnss_cb_->location_cbq_.reset();
    } else {
        aidl_gnss_cb_->sv_info_list_cbq_.reset();
        aidl_gnss_cb_->location_cbq_.reset();
    }
    StartAndCheckLocations(kLocationsToAwait);

    // Tolerate 1 less sv status to handle edge cases in reporting.
    int sv_info_list_cbq_size = (aidl_gnss_hal_->getInterfaceVersion() <= 1)
                                        ? gnss_cb_->sv_info_list_cbq_.size()
                                        : aidl_gnss_cb_->sv_info_list_cbq_.size();
    EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
    ALOGD("Observed %d GnssSvInfo, while awaiting %d Locations", sv_info_list_cbq_size,
          kLocationsToAwait);
    for (int i = 0; i < sv_info_list_cbq_size; ++i) {
        if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
            hidl_vec<IGnssCallback_2_1::GnssSvInfo> sv_info_vec;
            gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
            for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                const auto& gnss_sv = sv_info_vec[iSv];
                EXPECT_FALSE(
                        (static_cast<GnssConstellationType>(gnss_sv.v2_0.constellation) ==
                         source_to_blocklist_1.constellation) &&
                        (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
                EXPECT_FALSE(
                        (static_cast<GnssConstellationType>(gnss_sv.v2_0.constellation) ==
                         source_to_blocklist_2.constellation) &&
                        (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
            }
        } else {
            std::vector<IGnssCallback::GnssSvInfo> sv_info_vec;
            aidl_gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
            for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                const auto& gnss_sv = sv_info_vec[iSv];
                EXPECT_FALSE((gnss_sv.constellation == source_to_blocklist_1.constellation) &&
                             (gnss_sv.svFlag & (int)IGnssCallback::GnssSvFlags::USED_IN_FIX));
                EXPECT_FALSE((gnss_sv.constellation == source_to_blocklist_2.constellation) &&
                             (gnss_sv.svFlag & (int)IGnssCallback::GnssSvFlags::USED_IN_FIX));
            }
        }
    }

    // clean up
    StopAndClearLocations();
    sources.resize(0);
    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());
}

/*
 * BlocklistConstellationLocationOn:
 *
 * 1) Turns on location, waits for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus for any blockable constellations.
 * 2a & b) Blocklist first blockable constellation, and turn off location.
 * 3) Restart location, wait for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus does not use any constellation but GPS.
 * 4a & b) Clean up by turning off location, and send in empty blocklist.
 */
TEST_P(GnssHalTest, BlocklistConstellationLocationOn) {
    if (!(aidl_gnss_cb_->last_capabilities_ &
          (int)GnssCallbackAidl::CAPABILITY_SATELLITE_BLOCKLIST)) {
        ALOGI("Test BlocklistConstellationLocationOn skipped. SATELLITE_BLOCKLIST capability not "
              "supported.");
        return;
    }

    const int kLocationsToAwait = 3;
    const int kGnssSvInfoListTimeout = 2;

    // Find first blockable constellation to blocklist
    GnssConstellationType constellation_to_blocklist = static_cast<GnssConstellationType>(
            startLocationAndGetBlockableConstellation(kLocationsToAwait, kGnssSvInfoListTimeout));

    BlocklistedSource source_to_blocklist_1;
    source_to_blocklist_1.constellation = constellation_to_blocklist;
    source_to_blocklist_1.svid = 0;  // documented wildcard for all satellites in this constellation

    // IRNSS was added in 2.0. Always attempt to blocklist IRNSS to verify that the new enum is
    // supported.
    BlocklistedSource source_to_blocklist_2;
    source_to_blocklist_2.constellation = GnssConstellationType::IRNSS;
    source_to_blocklist_2.svid = 0;  // documented wildcard for all satellites in this constellation

    sp<IGnssConfiguration> gnss_configuration_hal;
    auto status = aidl_gnss_hal_->getExtensionGnssConfiguration(&gnss_configuration_hal);
    ASSERT_TRUE(status.isOk());
    ASSERT_NE(gnss_configuration_hal, nullptr);

    hidl_vec<BlocklistedSource> sources;
    sources.resize(2);
    sources[0] = source_to_blocklist_1;
    sources[1] = source_to_blocklist_2;

    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());

    // Turns off location
    StopAndClearLocations();

    // retry and ensure constellation not used
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        gnss_cb_->sv_info_list_cbq_.reset();
        gnss_cb_->location_cbq_.reset();
    } else {
        aidl_gnss_cb_->sv_info_list_cbq_.reset();
        aidl_gnss_cb_->location_cbq_.reset();
    }
    StartAndCheckLocations(kLocationsToAwait);

    // Tolerate 1 less sv status to handle edge cases in reporting.
    int sv_info_list_cbq_size = (aidl_gnss_hal_->getInterfaceVersion() <= 1)
                                        ? gnss_cb_->sv_info_list_cbq_.size()
                                        : aidl_gnss_cb_->sv_info_list_cbq_.size();
    EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
    ALOGD("Observed %d GnssSvInfo, while awaiting %d Locations", sv_info_list_cbq_size,
          kLocationsToAwait);
    for (int i = 0; i < sv_info_list_cbq_size; ++i) {
        if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
            hidl_vec<IGnssCallback_2_1::GnssSvInfo> sv_info_vec;
            gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
            for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                const auto& gnss_sv = sv_info_vec[iSv];
                EXPECT_FALSE(
                        (static_cast<GnssConstellationType>(gnss_sv.v2_0.constellation) ==
                         source_to_blocklist_1.constellation) &&
                        (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
                EXPECT_FALSE(
                        (static_cast<GnssConstellationType>(gnss_sv.v2_0.constellation) ==
                         source_to_blocklist_2.constellation) &&
                        (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
            }
        } else {
            std::vector<IGnssCallback::GnssSvInfo> sv_info_vec;
            aidl_gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
            for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                const auto& gnss_sv = sv_info_vec[iSv];
                EXPECT_FALSE((gnss_sv.constellation == source_to_blocklist_1.constellation) &&
                             (gnss_sv.svFlag & (int)IGnssCallback::GnssSvFlags::USED_IN_FIX));
                EXPECT_FALSE((gnss_sv.constellation == source_to_blocklist_2.constellation) &&
                             (gnss_sv.svFlag & (int)IGnssCallback::GnssSvFlags::USED_IN_FIX));
            }
        }
    }

    // clean up
    StopAndClearLocations();
    sources.resize(0);
    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());
}

/*
 * TestAllExtensions.
 */
TEST_P(GnssHalTest, TestAllExtensions) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }

    sp<IGnssBatching> iGnssBatching;
    auto status = aidl_gnss_hal_->getExtensionGnssBatching(&iGnssBatching);
    if (status.isOk() && iGnssBatching != nullptr) {
        auto gnssBatchingCallback = sp<GnssBatchingCallback>::make();
        status = iGnssBatching->init(gnssBatchingCallback);
        ASSERT_TRUE(status.isOk());

        status = iGnssBatching->cleanup();
        ASSERT_TRUE(status.isOk());
    }

    sp<IGnssGeofence> iGnssGeofence;
    status = aidl_gnss_hal_->getExtensionGnssGeofence(&iGnssGeofence);
    if (status.isOk() && iGnssGeofence != nullptr) {
        auto gnssGeofenceCallback = sp<GnssGeofenceCallback>::make();
        status = iGnssGeofence->setCallback(gnssGeofenceCallback);
        ASSERT_TRUE(status.isOk());
    }

    sp<IGnssNavigationMessageInterface> iGnssNavMsgIface;
    status = aidl_gnss_hal_->getExtensionGnssNavigationMessage(&iGnssNavMsgIface);
    if (status.isOk() && iGnssNavMsgIface != nullptr) {
        auto gnssNavMsgCallback = sp<GnssNavigationMessageCallback>::make();
        status = iGnssNavMsgIface->setCallback(gnssNavMsgCallback);
        ASSERT_TRUE(status.isOk());

        status = iGnssNavMsgIface->close();
        ASSERT_TRUE(status.isOk());
    }
}

/*
 * TestAGnssExtension:
 * 1. Gets the IAGnss extension.
 * 2. Sets AGnssCallback.
 * 3. Sets SUPL server host/port.
 */
TEST_P(GnssHalTest, TestAGnssExtension) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    sp<IAGnss> iAGnss;
    auto status = aidl_gnss_hal_->getExtensionAGnss(&iAGnss);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iAGnss != nullptr);

    auto agnssCallback = sp<AGnssCallbackAidl>::make();
    status = iAGnss->setCallback(agnssCallback);
    ASSERT_TRUE(status.isOk());

    // Set SUPL server host/port
    status = iAGnss->setServer(AGnssType::SUPL, std::string("supl.google.com"), 7275);
    ASSERT_TRUE(status.isOk());
}

/*
 * TestAGnssRilExtension:
 * 1. Gets the IAGnssRil extension.
 * 2. Sets AGnssRilCallback.
 * 3. Update network state to connected and then disconnected.
 * 4. Sets reference location.
 * 5. Injects empty NI message data and verifies that it returns an error.
 */
TEST_P(GnssHalTest, TestAGnssRilExtension) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    sp<IAGnssRil> iAGnssRil;
    auto status = aidl_gnss_hal_->getExtensionAGnssRil(&iAGnssRil);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iAGnssRil != nullptr);

    auto agnssRilCallback = sp<AGnssRilCallbackAidl>::make();
    status = iAGnssRil->setCallback(agnssRilCallback);
    ASSERT_TRUE(status.isOk());

    // Update GNSS HAL that a network has connected.
    IAGnssRil::NetworkAttributes networkAttributes;
    networkAttributes.networkHandle = 7700664333L;
    networkAttributes.isConnected = true;
    networkAttributes.capabilities = IAGnssRil::NETWORK_CAPABILITY_NOT_ROAMING;
    networkAttributes.apn = "placeholder-apn";
    status = iAGnssRil->updateNetworkState(networkAttributes);
    ASSERT_TRUE(status.isOk());

    // Update GNSS HAL that network has disconnected.
    networkAttributes.isConnected = false;
    status = iAGnssRil->updateNetworkState(networkAttributes);
    ASSERT_TRUE(status.isOk());

    // Set RefLocation
    IAGnssRil::AGnssRefLocationCellID agnssReflocationCellId;
    agnssReflocationCellId.type = IAGnssRil::AGnssRefLocationType::LTE_CELLID;
    agnssReflocationCellId.mcc = 466;
    agnssReflocationCellId.mnc = 97;
    agnssReflocationCellId.lac = 46697;
    agnssReflocationCellId.cid = 59168142;
    agnssReflocationCellId.pcid = 420;
    agnssReflocationCellId.tac = 11460;
    IAGnssRil::AGnssRefLocation agnssReflocation;
    agnssReflocation.type = IAGnssRil::AGnssRefLocationType::LTE_CELLID;
    agnssReflocation.cellID = agnssReflocationCellId;

    status = iAGnssRil->setRefLocation(agnssReflocation);
    ASSERT_TRUE(status.isOk());

    if (aidl_gnss_hal_->getInterfaceVersion() >= 3) {
        status = iAGnssRil->injectNiSuplMessageData(std::vector<uint8_t>(), 0);
        ASSERT_FALSE(status.isOk());
    }
}

/*
 * GnssDebugValuesSanityTest:
 * Ensures that GnssDebug values make sense.
 */
TEST_P(GnssHalTest, GnssDebugValuesSanityTest) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    sp<IGnssDebug> iGnssDebug;
    auto status = aidl_gnss_hal_->getExtensionGnssDebug(&iGnssDebug);
    ASSERT_TRUE(status.isOk());
    if (IsAutomotiveDevice()) {
        return;
    }
    ASSERT_TRUE(iGnssDebug != nullptr);

    IGnssDebug::DebugData data;
    status = iGnssDebug->getDebugData(&data);
    ASSERT_TRUE(status.isOk());
    Utils::checkPositionDebug(data);

    // Additional GnssDebug tests for AIDL version >= 4 (launched in Android 15(V)+)
    if (aidl_gnss_hal_->getInterfaceVersion() <= 3) {
        return;
    }

    // Start location and check the consistency between SvStatus and DebugData
    aidl_gnss_cb_->location_cbq_.reset();
    aidl_gnss_cb_->sv_info_list_cbq_.reset();
    StartAndCheckLocations(/* count= */ 2);
    int location_called_count = aidl_gnss_cb_->location_cbq_.calledCount();
    ALOGD("Observed %d GnssSvStatus, while awaiting 2 locations (%d received)",
          aidl_gnss_cb_->sv_info_list_cbq_.size(), location_called_count);

    // Wait for up to kNumSvInfoLists events for kTimeoutSeconds for each event.
    int kTimeoutSeconds = 2;
    int kNumSvInfoLists = 4;
    std::list<std::vector<IGnssCallback::GnssSvInfo>> sv_info_lists;
    std::vector<IGnssCallback::GnssSvInfo> last_sv_info_list;

    do {
        EXPECT_GT(aidl_gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_lists, kNumSvInfoLists,
                                                            kTimeoutSeconds),
                  0);
        if (!sv_info_lists.empty()) {
            last_sv_info_list = sv_info_lists.back();
            ALOGD("last_sv_info size = %d", (int)last_sv_info_list.size());
        }
    } while (!sv_info_lists.empty() && last_sv_info_list.size() == 0);

    StopAndClearLocations();

    status = iGnssDebug->getDebugData(&data);
    Utils::checkPositionDebug(data);

    // Validate SatelliteEphemerisType, SatelliteEphemerisSource, SatelliteEphemerisHealth
    for (auto sv_info : last_sv_info_list) {
        if ((sv_info.svFlag & static_cast<int>(IGnssCallback::GnssSvFlags::USED_IN_FIX)) == 0) {
            continue;
        }
        ALOGD("Found usedInFix const: %d, svid: %d", static_cast<int>(sv_info.constellation),
              sv_info.svid);
        bool foundDebugData = false;
        for (auto satelliteData : data.satelliteDataArray) {
            if (satelliteData.constellation == sv_info.constellation &&
                satelliteData.svid == sv_info.svid) {
                foundDebugData = true;
                ALOGD("Found GnssDebug data for this sv.");
                EXPECT_TRUE(satelliteData.serverPredictionIsAvailable ||
                            satelliteData.ephemerisType ==
                                    IGnssDebug::SatelliteEphemerisType::EPHEMERIS);
                // for satellites with ephType=0, they need ephHealth=0 if used-in-fix
                if (satelliteData.ephemerisType == IGnssDebug::SatelliteEphemerisType::EPHEMERIS) {
                    EXPECT_TRUE(satelliteData.ephemerisHealth ==
                                IGnssDebug::SatelliteEphemerisHealth::GOOD);
                }
                break;
            }
        }
        // Every Satellite where GnssStatus says it is used-in-fix has a valid ephemeris - i.e. it's
        // it shows either a serverPredAvail: 1, or a ephType=0
        EXPECT_TRUE(foundDebugData);
    }

    bool hasServerPredictionAvailable = false;
    bool hasNoneZeroServerPredictionAgeSeconds = false;
    bool hasNoneDemodEphSource = false;
    for (auto satelliteData : data.satelliteDataArray) {
        // for satellites with serverPredAvail: 1, the serverPredAgeSec: is not 0 for all
        // satellites (at least not on 2 fixes in a row - it could get lucky once)
        if (satelliteData.serverPredictionIsAvailable) {
            hasServerPredictionAvailable = true;
            if (satelliteData.serverPredictionAgeSeconds != 0) {
                hasNoneZeroServerPredictionAgeSeconds = true;
            }
        }
        // for satellites with ephType=0, they need ephSource 0-3
        if (satelliteData.ephemerisType == IGnssDebug::SatelliteEphemerisType::EPHEMERIS) {
            EXPECT_TRUE(satelliteData.ephemerisSource >=
                                SatellitePvt::SatelliteEphemerisSource::DEMODULATED &&
                        satelliteData.ephemerisSource <=
                                SatellitePvt::SatelliteEphemerisSource::OTHER);
            if (satelliteData.ephemerisSource !=
                SatellitePvt::SatelliteEphemerisSource::DEMODULATED) {
                hasNoneDemodEphSource = true;
            }
        }
    }
    if (hasNoneDemodEphSource && hasServerPredictionAvailable) {
        EXPECT_TRUE(hasNoneZeroServerPredictionAgeSeconds);
    }

    /**
    - Gnss Location Data:: should show some valid information, ideally reasonably close (+/-1km) to
        the Location output - at least after the 2nd valid location output (maybe in general, wait
        for 2 good Location outputs before checking this, in case they don't update the assistance
        until after they output the Location)
    */
    double distanceM =
            Utils::distanceMeters(data.position.latitudeDegrees, data.position.longitudeDegrees,
                                  aidl_gnss_cb_->last_location_.latitudeDegrees,
                                  aidl_gnss_cb_->last_location_.longitudeDegrees);
    ALOGD("distance between debug position and last position: %.2lf", distanceM);
    EXPECT_LT(distanceM, 1000.0);  // 1km

    /**
    - Gnss Time Data:: timeEstimate should be reasonably close to the current GPS time.
    - Gnss Time Data:: timeUncertaintyNs should always be > 0 and < 5e9 (could be large due
        to solve-for-time type solutions)
    - Gnss Time Data:: frequencyUncertaintyNsPerSec: should always be > 0 and < 1000 (1000 ns/s
        corresponds to roughly a 300 m/s speed error, which should be pretty rare)
    */
    ALOGD("debug time: %" PRId64 ", position time: %" PRId64, data.time.timeEstimateMs,
          aidl_gnss_cb_->last_location_.timestampMillis);
    // Allowing 5s between the last location time and the current GPS time
    EXPECT_LT(abs(data.time.timeEstimateMs - aidl_gnss_cb_->last_location_.timestampMillis), 5000);

    ALOGD("debug time uncertainty: %f ns", data.time.timeUncertaintyNs);
    EXPECT_GT(data.time.timeUncertaintyNs, 0);
    EXPECT_LT(data.time.timeUncertaintyNs, 5e9);

    ALOGD("debug freq uncertainty: %f ns/s", data.time.frequencyUncertaintyNsPerSec);
    EXPECT_GT(data.time.frequencyUncertaintyNsPerSec, 0);
    EXPECT_LT(data.time.frequencyUncertaintyNsPerSec, 1000);
}

/*
 * TestGnssVisibilityControlExtension:
 * 1. Gets the IGnssVisibilityControl extension.
 * 2. Sets GnssVisibilityControlCallback
 * 3. Sets proxy apps
 */
TEST_P(GnssHalTest, TestGnssVisibilityControlExtension) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    sp<IGnssVisibilityControl> iGnssVisibilityControl;
    auto status = aidl_gnss_hal_->getExtensionGnssVisibilityControl(&iGnssVisibilityControl);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssVisibilityControl != nullptr);
    auto gnssVisibilityControlCallback = sp<GnssVisibilityControlCallback>::make();
    status = iGnssVisibilityControl->setCallback(gnssVisibilityControlCallback);
    ASSERT_TRUE(status.isOk());

    std::vector<std::string> proxyApps{std::string("com.example.ims"),
                                       std::string("com.example.mdt")};
    status = iGnssVisibilityControl->enableNfwLocationAccess(proxyApps);
    ASSERT_TRUE(status.isOk());
}

/*
 * TestGnssAgcInGnssMeasurement:
 * 1. Gets the GnssMeasurementExtension and verifies that it returns a non-null extension.
 * 2. Sets a GnssMeasurementCallback, waits for a measurement.
 */
TEST_P(GnssHalTest, TestGnssAgcInGnssMeasurement) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    const int kFirstGnssMeasurementTimeoutSeconds = 10;
    const int kNumMeasurementEvents = 5;

    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    auto callback = sp<GnssMeasurementCallbackAidl>::make();
    status = iGnssMeasurement->setCallback(callback, /* enableFullTracking= */ false,
                                           /* enableCorrVecOutputs */ false);
    ASSERT_TRUE(status.isOk());

    for (int i = 0; i < kNumMeasurementEvents; i++) {
        GnssData lastMeasurement;
        ASSERT_TRUE(callback->gnss_data_cbq_.retrieve(lastMeasurement,
                                                      kFirstGnssMeasurementTimeoutSeconds));
        EXPECT_EQ(callback->gnss_data_cbq_.calledCount(), i + 1);
        if (i > 2) {
            // Allow 3 seconds tolerance for empty measurement
            ASSERT_TRUE(lastMeasurement.measurements.size() > 0);
        }

        // Validity check GnssData fields
        checkGnssMeasurementClockFields(lastMeasurement);

        ASSERT_TRUE(lastMeasurement.gnssAgcs.size() > 0);
        for (const auto& gnssAgc : lastMeasurement.gnssAgcs) {
            ASSERT_TRUE(gnssAgc.carrierFrequencyHz >= 0);
        }
    }

    status = iGnssMeasurement->close();
    ASSERT_TRUE(status.isOk());
}

/*
 * TestGnssAntennaInfo:
 * Sets a GnssAntennaInfoCallback, waits for report, and verifies
 * 1. phaseCenterOffsetCoordinateMillimeters is valid
 * 2. phaseCenterOffsetCoordinateUncertaintyMillimeters is valid.
 * PhaseCenterVariationCorrections and SignalGainCorrections are optional.
 */
TEST_P(GnssHalTest, TestGnssAntennaInfo) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }

    const int kAntennaInfoTimeoutSeconds = 2;
    sp<IGnssAntennaInfo> iGnssAntennaInfo;
    auto status = aidl_gnss_hal_->getExtensionGnssAntennaInfo(&iGnssAntennaInfo);
    ASSERT_TRUE(status.isOk());

    if (!(aidl_gnss_cb_->last_capabilities_ & (int)GnssCallbackAidl::CAPABILITY_ANTENNA_INFO) ||
        iGnssAntennaInfo == nullptr) {
        ALOGD("GnssAntennaInfo AIDL is not supported.");
        return;
    }

    auto callback = sp<GnssAntennaInfoCallbackAidl>::make();
    status = iGnssAntennaInfo->setCallback(callback);
    ASSERT_TRUE(status.isOk());

    std::vector<IGnssAntennaInfoCallback::GnssAntennaInfo> antennaInfos;
    ASSERT_TRUE(callback->antenna_info_cbq_.retrieve(antennaInfos, kAntennaInfoTimeoutSeconds));
    EXPECT_EQ(callback->antenna_info_cbq_.calledCount(), 1);
    ASSERT_TRUE(antennaInfos.size() > 0);

    for (auto antennaInfo : antennaInfos) {
        // Remaining fields are optional
        if (!antennaInfo.phaseCenterVariationCorrectionMillimeters.empty()) {
            int numRows = antennaInfo.phaseCenterVariationCorrectionMillimeters.size();
            int numColumns = antennaInfo.phaseCenterVariationCorrectionMillimeters[0].row.size();
            // Must have at least 1 row and 2 columns
            ASSERT_TRUE(numRows >= 1 && numColumns >= 2);

            // Corrections and uncertainties must have same dimensions
            ASSERT_TRUE(antennaInfo.phaseCenterVariationCorrectionMillimeters.size() ==
                        antennaInfo.phaseCenterVariationCorrectionUncertaintyMillimeters.size());
            ASSERT_TRUE(
                    antennaInfo.phaseCenterVariationCorrectionMillimeters[0].row.size() ==
                    antennaInfo.phaseCenterVariationCorrectionUncertaintyMillimeters[0].row.size());

            // Must be rectangular
            for (auto row : antennaInfo.phaseCenterVariationCorrectionMillimeters) {
                ASSERT_TRUE(row.row.size() == numColumns);
            }
            for (auto row : antennaInfo.phaseCenterVariationCorrectionUncertaintyMillimeters) {
                ASSERT_TRUE(row.row.size() == numColumns);
            }
        }
        if (!antennaInfo.signalGainCorrectionDbi.empty()) {
            int numRows = antennaInfo.signalGainCorrectionDbi.size();
            int numColumns = antennaInfo.signalGainCorrectionUncertaintyDbi[0].row.size();
            // Must have at least 1 row and 2 columns
            ASSERT_TRUE(numRows >= 1 && numColumns >= 2);

            // Corrections and uncertainties must have same dimensions
            ASSERT_TRUE(antennaInfo.signalGainCorrectionDbi.size() ==
                        antennaInfo.signalGainCorrectionUncertaintyDbi.size());
            ASSERT_TRUE(antennaInfo.signalGainCorrectionDbi[0].row.size() ==
                        antennaInfo.signalGainCorrectionUncertaintyDbi[0].row.size());

            // Must be rectangular
            for (auto row : antennaInfo.signalGainCorrectionDbi) {
                ASSERT_TRUE(row.row.size() == numColumns);
            }
            for (auto row : antennaInfo.signalGainCorrectionUncertaintyDbi) {
                ASSERT_TRUE(row.row.size() == numColumns);
            }
        }
    }

    iGnssAntennaInfo->close();
}

/*
 * TestGnssMeasurementCorrections:
 * If measurement corrections capability is supported, verifies that the measurement corrections
 * capabilities are reported and the mandatory LOS_SATS or the EXCESS_PATH_LENGTH
 * capability flag is set.
 */
TEST_P(GnssHalTest, TestGnssMeasurementCorrections) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    if (!(aidl_gnss_cb_->last_capabilities_ &
          (int)GnssCallbackAidl::CAPABILITY_MEASUREMENT_CORRECTIONS)) {
        return;
    }

    sp<IMeasurementCorrectionsInterface> iMeasurementCorrectionsAidl;
    auto status = aidl_gnss_hal_->getExtensionMeasurementCorrections(&iMeasurementCorrectionsAidl);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iMeasurementCorrectionsAidl != nullptr);

    // Setup measurement corrections callback.
    auto gnssMeasurementCorrectionsCallback = sp<MeasurementCorrectionsCallback>::make();
    status = iMeasurementCorrectionsAidl->setCallback(gnssMeasurementCorrectionsCallback);
    ASSERT_TRUE(status.isOk());

    const int kTimeoutSec = 5;
    EXPECT_TRUE(gnssMeasurementCorrectionsCallback->capabilities_cbq_.retrieve(
            gnssMeasurementCorrectionsCallback->last_capabilities_, kTimeoutSec));
    ASSERT_TRUE(gnssMeasurementCorrectionsCallback->capabilities_cbq_.calledCount() > 0);

    ASSERT_TRUE((gnssMeasurementCorrectionsCallback->last_capabilities_ &
                 (MeasurementCorrectionsCallback::CAPABILITY_LOS_SATS |
                  MeasurementCorrectionsCallback::CAPABILITY_EXCESS_PATH_LENGTH)) != 0);

    // Set a mock MeasurementCorrections.
    status = iMeasurementCorrectionsAidl->setCorrections(
            Utils::getMockMeasurementCorrections_aidl());
    ASSERT_TRUE(status.isOk());
}

/*
 * TestStopSvStatusAndNmea:
 * 1. Call stopSvStatus and stopNmea.
 * 2. Start location and verify that
 *    - no SvStatus is received.
 *    - no Nmea is received.
 */
TEST_P(GnssHalTest, TestStopSvStatusAndNmea) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    auto status = aidl_gnss_hal_->stopSvStatus();
    EXPECT_TRUE(status.isOk());
    status = aidl_gnss_hal_->stopNmea();
    EXPECT_TRUE(status.isOk());

    int kLocationsToAwait = 5;
    aidl_gnss_cb_->location_cbq_.reset();
    aidl_gnss_cb_->sv_info_list_cbq_.reset();
    aidl_gnss_cb_->nmea_cbq_.reset();
    StartAndCheckLocations(/* count= */ kLocationsToAwait,
                           /* start_sv_status= */ false, /* start_nmea= */ false);
    int location_called_count = aidl_gnss_cb_->location_cbq_.calledCount();
    ALOGD("Observed %d GnssSvStatus, and %d Nmea while awaiting %d locations (%d received)",
          aidl_gnss_cb_->sv_info_list_cbq_.size(), aidl_gnss_cb_->nmea_cbq_.size(),
          kLocationsToAwait, location_called_count);

    // Ensure that no SvStatus & no Nmea is received.
    EXPECT_EQ(aidl_gnss_cb_->sv_info_list_cbq_.size(), 0);
    EXPECT_EQ(aidl_gnss_cb_->nmea_cbq_.size(), 0);

    StopAndClearLocations();
}

/*
 * TestGnssMeasurementIntervals_WithoutLocation:
 * 1. Start measurement at intervals
 * 2. Verify measurement are received at expected intervals
 * 3. Verify status are reported at expected intervals
 */
TEST_P(GnssHalTest, TestGnssMeasurementIntervals_WithoutLocation) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }

    std::vector<int> intervals({2000, 4000});
    std::vector<int> numEvents({10, 5});

    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    ALOGD("TestGnssMeasurementIntervals_WithoutLocation");
    for (int i = 0; i < intervals.size(); i++) {
        auto callback = sp<GnssMeasurementCallbackAidl>::make();
        startMeasurementWithInterval(intervals[i], iGnssMeasurement, callback);

        std::vector<int> measurementDeltas;
        std::vector<int> svInfoListDeltas;

        collectMeasurementIntervals(callback, numEvents[i], /* timeoutSeconds= */ 10,
                                    measurementDeltas);
        if (aidl_gnss_hal_->getInterfaceVersion() >= 3) {
            collectSvInfoListTimestamps(numEvents[i], /* timeoutSeconds= */ 10, svInfoListDeltas);
            EXPECT_TRUE(aidl_gnss_cb_->sv_info_list_cbq_.size() > 0);
        }
        status = iGnssMeasurement->close();
        ASSERT_TRUE(status.isOk());

        assertMeanAndStdev(intervals[i], measurementDeltas);

        if (aidl_gnss_hal_->getInterfaceVersion() >= 3) {
            assertMeanAndStdev(intervals[i], svInfoListDeltas);
        }
    }
}

/*
 * TestGnssMeasurementIntervals_LocationOnBeforeMeasurement:
 * 1. Start location at 1s.
 * 2. Start measurement at 2s. Verify measurements are received at 1s.
 * 3. Stop measurement. Stop location.
 */
TEST_P(GnssHalTest, TestGnssMeasurementIntervals_LocationOnBeforeMeasurement) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }

    std::vector<int> intervals({2000});

    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    int locationIntervalMs = 1000;

    // Start location first and then start measurement
    ALOGD("TestGnssMeasurementIntervals_LocationOnBeforeMeasurement");
    StartAndCheckFirstLocation(locationIntervalMs, /* lowPowerMode= */ false);
    for (auto& intervalMs : intervals) {
        auto callback = sp<GnssMeasurementCallbackAidl>::make();
        startMeasurementWithInterval(intervalMs, iGnssMeasurement, callback);

        std::vector<int> measurementDeltas;
        std::vector<int> svInfoListDeltas;

        collectMeasurementIntervals(callback, /*numEvents=*/10, /*timeoutSeconds=*/10,
                                    measurementDeltas);
        if (aidl_gnss_hal_->getInterfaceVersion() >= 3) {
            collectSvInfoListTimestamps(/*numEvents=*/10, /* timeoutSeconds= */ 10,
                                        svInfoListDeltas);
            EXPECT_TRUE(aidl_gnss_cb_->sv_info_list_cbq_.size() > 0);
        }

        status = iGnssMeasurement->close();
        ASSERT_TRUE(status.isOk());

        assertMeanAndStdev(locationIntervalMs, measurementDeltas);
        if (aidl_gnss_hal_->getInterfaceVersion() >= 3) {
            // Verify the SvStatus interval is 1s (not 2s)
            assertMeanAndStdev(locationIntervalMs, svInfoListDeltas);
        }
    }
    StopAndClearLocations();
}

/*
 * TestGnssMeasurementIntervals_LocationOnAfterMeasurement:
 * 1. Start measurement at 2s
 * 2. Start location at 1s. Verify measurements are received at 1s
 * 3. Stop location. Verify measurements are received at 2s
 * 4. Stop measurement
 */
TEST_P(GnssHalTest, TestGnssMeasurementIntervals_LocationOnAfterMeasurement) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 1) {
        return;
    }
    const int kFirstMeasTimeoutSec = 10;
    std::vector<int> intervals({2000});

    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    int locationIntervalMs = 1000;
    // Start measurement first and then start location
    ALOGD("TestGnssMeasurementIntervals_LocationOnAfterMeasurement");
    for (auto& intervalMs : intervals) {
        auto callback = sp<GnssMeasurementCallbackAidl>::make();
        startMeasurementWithInterval(intervalMs, iGnssMeasurement, callback);

        // Start location and verify the measurements are received at 1Hz
        StartAndCheckFirstLocation(locationIntervalMs, /* lowPowerMode= */ false);
        std::vector<int> measurementDeltas;
        std::vector<int> svInfoListDeltas;
        collectMeasurementIntervals(callback, /*numEvents=*/10, kFirstMeasTimeoutSec,
                                    measurementDeltas);
        assertMeanAndStdev(locationIntervalMs, measurementDeltas);
        if (aidl_gnss_hal_->getInterfaceVersion() >= 3) {
            collectSvInfoListTimestamps(/*numEvents=*/10, /* timeoutSeconds= */ 10,
                                        svInfoListDeltas);
            EXPECT_TRUE(aidl_gnss_cb_->sv_info_list_cbq_.size() > 0);
            // Verify the SvStatus intervals are at 1s interval
            assertMeanAndStdev(locationIntervalMs, svInfoListDeltas);
        }

        // Stop location request and verify the measurements are received at 2s intervals
        StopAndClearLocations();
        measurementDeltas.clear();
        collectMeasurementIntervals(callback, /*numEvents=*/5, kFirstMeasTimeoutSec,
                                    measurementDeltas);
        assertMeanAndStdev(intervalMs, measurementDeltas);

        if (aidl_gnss_hal_->getInterfaceVersion() >= 3) {
            svInfoListDeltas.clear();
            collectSvInfoListTimestamps(/*numEvents=*/5, /* timeoutSeconds= */ 10,
                                        svInfoListDeltas);
            EXPECT_TRUE(aidl_gnss_cb_->sv_info_list_cbq_.size() > 0);
            // Verify the SvStatus intervals are at 2s interval
            for (const int& delta : svInfoListDeltas) {
                ALOGD("svInfoListDelta: %d", delta);
            }
            assertMeanAndStdev(intervalMs, svInfoListDeltas);
        }

        status = iGnssMeasurement->close();
        ASSERT_TRUE(status.isOk());
    }
}

/*
 * TestGnssMeasurementIntervals_changeIntervals:
 * This test ensures setCallback() can be called consecutively without close().
 * 1. Start measurement with 20s interval and wait for 1 measurement.
 * 2. Start measurement with 1s interval and wait for 5 measurements.
 *    Verify the measurements were received at 1Hz.
 * 3. Start measurement with 2s interval and wait for 5 measurements.
 *    Verify the measurements were received at 2s intervals.
 */
TEST_P(GnssHalTest, TestGnssMeasurementIntervals_changeIntervals) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 2) {
        return;
    }
    const int kFirstGnssMeasurementTimeoutSeconds = 10;
    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    auto callback = sp<GnssMeasurementCallbackAidl>::make();
    std::vector<int> deltas;

    // setCallback at 20s interval and wait for 1 measurement
    startMeasurementWithInterval(20000, iGnssMeasurement, callback);
    collectMeasurementIntervals(callback, /* numEvents= */ 1, kFirstGnssMeasurementTimeoutSeconds,
                                deltas);

    // setCallback at 1s interval and wait for 5 measurements
    callback->gnss_data_cbq_.reset();
    deltas.clear();
    startMeasurementWithInterval(1000, iGnssMeasurement, callback);
    collectMeasurementIntervals(callback, /* numEvents= */ 5, kFirstGnssMeasurementTimeoutSeconds,
                                deltas);

    // verify the measurements were received at 1Hz
    assertMeanAndStdev(1000, deltas);

    // setCallback at 2s interval and wait for 5 measurements
    callback->gnss_data_cbq_.reset();
    deltas.clear();
    startMeasurementWithInterval(2000, iGnssMeasurement, callback);
    collectMeasurementIntervals(callback, /* numEvents= */ 5, kFirstGnssMeasurementTimeoutSeconds,
                                deltas);

    // verify the measurements were received at 2s intervals
    assertMeanAndStdev(2000, deltas);

    status = iGnssMeasurement->close();
    ASSERT_TRUE(status.isOk());
}

/*
 * TestGnssMeasurementIsFullTracking
 * 1. Start measurement with enableFullTracking=true. Verify the received measurements have
 *    isFullTracking=true.
 * 2. Start measurement with enableFullTracking = false.
 * 3. Do step 1 again.
 */
TEST_P(GnssHalTest, TestGnssMeasurementIsFullTracking) {
    // GnssData.isFullTracking is added in the interface version 3
    if (aidl_gnss_hal_->getInterfaceVersion() <= 2) {
        return;
    }
    const int kFirstGnssMeasurementTimeoutSeconds = 10;
    const int kNumMeasurementEvents = 5;
    std::vector<bool> isFullTrackingList({true, false, true});

    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    ALOGD("TestGnssMeasurementIsFullTracking");
    auto callback = sp<GnssMeasurementCallbackAidl>::make();
    IGnssMeasurementInterface::Options options;
    options.intervalMs = 1000;

    for (auto isFullTracking : isFullTrackingList) {
        options.enableFullTracking = isFullTracking;

        callback->gnss_data_cbq_.reset();
        auto status = iGnssMeasurement->setCallbackWithOptions(callback, options);
        checkGnssDataFields(callback, kNumMeasurementEvents, kFirstGnssMeasurementTimeoutSeconds,
                            isFullTracking);
    }

    status = iGnssMeasurement->close();
    ASSERT_TRUE(status.isOk());
}

/*
 * TestAccumulatedDeltaRange:
 * 1. Gets the GnssMeasurementExtension and verifies that it returns a non-null extension.
 * 2. Start measurement with 1s interval and wait for up to 15 measurements.
 * 3. Verify at least one measurement has a valid AccumulatedDeltaRange state.
 */
TEST_P(GnssHalTest, TestAccumulatedDeltaRange) {
    if (aidl_gnss_hal_->getInterfaceVersion() <= 2) {
        return;
    }
    if ((aidl_gnss_cb_->last_capabilities_ & IGnssCallback::CAPABILITY_ACCUMULATED_DELTA_RANGE) ==
        0) {
        return;
    }

    ALOGD("TestAccumulatedDeltaRange");

    auto callback = sp<GnssMeasurementCallbackAidl>::make();
    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    IGnssMeasurementInterface::Options options;
    options.intervalMs = 1000;
    options.enableFullTracking = true;
    status = iGnssMeasurement->setCallbackWithOptions(callback, options);
    ASSERT_TRUE(status.isOk());

    bool accumulatedDeltaRangeFound = false;
    const int kNumMeasurementEvents = 15;

    // setCallback at 1s interval and wait for 15 measurements
    for (int i = 0; i < kNumMeasurementEvents; i++) {
        GnssData lastGnssData;
        ASSERT_TRUE(callback->gnss_data_cbq_.retrieve(lastGnssData, 10));
        EXPECT_EQ(callback->gnss_data_cbq_.calledCount(), i + 1);
        if (i <= 2 && lastGnssData.measurements.size() == 0) {
            // Allow 3 seconds tolerance to report empty measurement
            continue;
        }
        ASSERT_TRUE(lastGnssData.measurements.size() > 0);

        // Validity check GnssData fields
        checkGnssMeasurementClockFields(lastGnssData);
        for (const auto& measurement : lastGnssData.measurements) {
            if ((measurement.accumulatedDeltaRangeState & measurement.ADR_STATE_VALID) > 0) {
                accumulatedDeltaRangeFound = true;
                break;
            }
        }
        if (accumulatedDeltaRangeFound) break;
    }
    ASSERT_TRUE(accumulatedDeltaRangeFound);
    status = iGnssMeasurement->close();
    ASSERT_TRUE(status.isOk());
}

/*
 * TestSvStatusIntervals:
 * 1. start measurement and location with various intervals
 * 2. verify the SvStatus are received at expected interval
 */
TEST_P(GnssHalTest, TestSvStatusIntervals) {
    // Only runs on devices launched in Android 15+
    if (aidl_gnss_hal_->getInterfaceVersion() <= 3) {
        return;
    }
    ALOGD("TestSvStatusIntervals");
    sp<IGnssMeasurementInterface> iGnssMeasurement;
    auto status = aidl_gnss_hal_->getExtensionGnssMeasurement(&iGnssMeasurement);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssMeasurement != nullptr);

    std::vector<int> locationIntervals{1000, 2000, INT_MAX};
    std::vector<int> measurementIntervals{1000, 2000, INT_MAX};

    for (auto& locationIntervalMs : locationIntervals) {
        for (auto& measurementIntervalMs : measurementIntervals) {
            if (locationIntervalMs == INT_MAX && measurementIntervalMs == INT_MAX) {
                continue;
            }
            auto measurementCallback = sp<GnssMeasurementCallbackAidl>::make();
            // Start measurement
            if (measurementIntervalMs < INT_MAX) {
                startMeasurementWithInterval(measurementIntervalMs, iGnssMeasurement,
                                             measurementCallback);
            }
            // Start location
            if (locationIntervalMs < INT_MAX) {
                StartAndCheckFirstLocation(locationIntervalMs, /* lowPowerMode= */ false);
            }
            ALOGD("location@%d(ms), measurement@%d(ms)", locationIntervalMs, measurementIntervalMs);
            std::vector<int> svInfoListDeltas;
            collectSvInfoListTimestamps(/*numEvents=*/5, /* timeoutSeconds= */ 10,
                                        svInfoListDeltas);
            EXPECT_TRUE(aidl_gnss_cb_->sv_info_list_cbq_.size() > 0);

            int svStatusInterval = std::min(locationIntervalMs, measurementIntervalMs);
            assertMeanAndStdev(svStatusInterval, svInfoListDeltas);

            if (locationIntervalMs < INT_MAX) {
                // Stop location request
                StopAndClearLocations();
            }
            if (measurementIntervalMs < INT_MAX) {
                // Stop measurement request
                status = iGnssMeasurement->close();
                ASSERT_TRUE(status.isOk());
            }
        }
    }
}

/*
 * Test GnssAssistanceExtension:
 * 1. Gets the GnssAssistanceExtension
 * 2. Injects empty GnssAssistance data and verifies that it returns an error.
 * 3. Injects non-empty GnssAssistance data and verifies that a success status is returned.
 */
TEST_P(GnssHalTest, TestGnssAssistanceExtension) {
    // Only runs on devices launched in Android 16+
    if (aidl_gnss_hal_->getInterfaceVersion() <= 5) {
        return;
    }
    sp<IGnssAssistanceInterface> iGnssAssistance;
    auto status = aidl_gnss_hal_->getExtensionGnssAssistanceInterface(&iGnssAssistance);
    if (status.isOk() && iGnssAssistance != nullptr) {
        GnssAssistance emptyGnssAssistance;
        status = iGnssAssistance->injectGnssAssistance(emptyGnssAssistance);
        ASSERT_FALSE(status.isOk());

        GnssAssistance nonEmptyGnssAssistance;
        nonEmptyGnssAssistance.gpsAssistance.emplace();
        nonEmptyGnssAssistance.gpsAssistance->satelliteEphemeris.emplace_back();
        status = iGnssAssistance->injectGnssAssistance(nonEmptyGnssAssistance);
        ASSERT_TRUE(status.isOk());
    }
}