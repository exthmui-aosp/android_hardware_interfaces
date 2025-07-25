/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "SubscriptionManager.h"

#include <VehicleUtils.h>
#include <android-base/format.h>
#include <android-base/stringprintf.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>

#include <inttypes.h>

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {

namespace {

using ::aidl::android::hardware::automotive::vehicle::IVehicleCallback;
using ::aidl::android::hardware::automotive::vehicle::StatusCode;
using ::aidl::android::hardware::automotive::vehicle::SubscribeOptions;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropError;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropValue;
using ::android::base::Error;
using ::android::base::Result;
using ::android::base::StringPrintf;
using ::ndk::ScopedAStatus;

constexpr float EPSILON = 0.0000001;
constexpr float ONE_SECOND_IN_NANOS = 1'000'000'000.;

SubscribeOptions newSubscribeOptions(int32_t propId, int32_t areaId, float sampleRateHz,
                                     float resolution, bool enableVur) {
    SubscribeOptions subscribedOptions;
    subscribedOptions.propId = propId;
    subscribedOptions.areaIds = {areaId};
    subscribedOptions.sampleRate = sampleRateHz;
    subscribedOptions.resolution = resolution;
    subscribedOptions.enableVariableUpdateRate = enableVur;

    return subscribedOptions;
}

}  // namespace

SubscriptionManager::SubscriptionManager(IVehicleHardware* vehicleHardware)
    : mVehicleHardware(vehicleHardware) {}

SubscriptionManager::~SubscriptionManager() {
    std::scoped_lock<std::mutex> lockGuard(mLock);

    mClientsByPropIdAreaId.clear();
    mSubscribedPropsByClient.clear();
    mSupportedValueChangeClientsByPropIdAreaId.clear();
    mSupportedValueChangePropIdAreaIdsByClient.clear();
}

bool SubscriptionManager::checkSampleRateHz(float sampleRateHz) {
    return getIntervalNanos(sampleRateHz).ok();
}

Result<int64_t> SubscriptionManager::getIntervalNanos(float sampleRateHz) {
    int64_t intervalNanos = 0;
    if (sampleRateHz <= 0) {
        return Error() << "invalid sample rate, must be a positive number";
    }
    if (sampleRateHz <= (ONE_SECOND_IN_NANOS / static_cast<float>(INT64_MAX))) {
        return Error() << "invalid sample rate: " << sampleRateHz << ", too small";
    }
    intervalNanos = static_cast<int64_t>(ONE_SECOND_IN_NANOS / sampleRateHz);
    return intervalNanos;
}

bool SubscriptionManager::checkResolution(float resolution) {
    if (resolution == 0) {
        return true;
    }

    float log = std::log10(resolution);
    return std::abs(log - std::round(log)) < EPSILON;
}

void ContSubConfigs::refreshCombinedConfig() {
    float maxSampleRateHz = 0.;
    float minRequiredResolution = std::numeric_limits<float>::max();
    bool enableVur = true;
    // This is not called frequently so a brute-focre is okay. More efficient way exists but this
    // is simpler.
    for (const auto& [_, subConfig] : mConfigByClient) {
        if (subConfig.sampleRateHz > maxSampleRateHz) {
            maxSampleRateHz = subConfig.sampleRateHz;
        }
        if (subConfig.resolution < minRequiredResolution) {
            minRequiredResolution = subConfig.resolution;
        }
        if (!subConfig.enableVur) {
            // If one client does not enable variable update rate, we cannot enable variable update
            // rate in IVehicleHardware.
            enableVur = false;
        }
    }
    mMaxSampleRateHz = maxSampleRateHz;
    mMinRequiredResolution = minRequiredResolution;
    mEnableVur = enableVur;
}

void ContSubConfigs::addClient(const ClientIdType& clientId, const SubConfig& subConfig) {
    mConfigByClient[clientId] = subConfig;
    refreshCombinedConfig();
}

void ContSubConfigs::removeClient(const ClientIdType& clientId) {
    mConfigByClient.erase(clientId);
    refreshCombinedConfig();
}

float ContSubConfigs::getMaxSampleRateHz() const {
    return mMaxSampleRateHz;
}

float ContSubConfigs::getMinRequiredResolution() const {
    return mMinRequiredResolution;
}

bool ContSubConfigs::isVurEnabled() const {
    return mEnableVur;
}

bool ContSubConfigs::isVurEnabledForClient(const ClientIdType& clientId) const {
    if (mConfigByClient.find(clientId) == mConfigByClient.end()) {
        return false;
    }
    return mConfigByClient.at(clientId).enableVur;
}

float ContSubConfigs::getResolutionForClient(const ClientIdType& clientId) const {
    if (mConfigByClient.find(clientId) == mConfigByClient.end()) {
        return 0.0f;
    }
    return mConfigByClient.at(clientId).resolution;
}

VhalResult<void> SubscriptionManager::addOnChangeSubscriberLocked(
        const PropIdAreaId& propIdAreaId) {
    if (mClientsByPropIdAreaId.find(propIdAreaId) != mClientsByPropIdAreaId.end()) {
        // This propId, areaId is already subscribed, ignore the request.
        return {};
    }

    int32_t propId = propIdAreaId.propId;
    int32_t areaId = propIdAreaId.areaId;
    if (auto status = mVehicleHardware->subscribe(
                newSubscribeOptions(propId, areaId, /*updateRateHz=*/0, /*resolution*/ 0.0f,
                                    /*enableVur*/ false));
        status != StatusCode::OK) {
        return StatusError(status)
               << fmt::format("failed subscribe for propIdAreaId: {}", propIdAreaId);
    }
    return {};
}

VhalResult<void> SubscriptionManager::addContinuousSubscriberLocked(
        const ClientIdType& clientId, const PropIdAreaId& propIdAreaId, float sampleRateHz,
        float resolution, bool enableVur) {
    // Make a copy so that we don't modify 'mContSubConfigsByPropIdArea' on failure cases.
    ContSubConfigs newConfig = mContSubConfigsByPropIdArea[propIdAreaId];
    SubConfig subConfig = {
            .sampleRateHz = sampleRateHz,
            .resolution = resolution,
            .enableVur = enableVur,
    };
    newConfig.addClient(clientId, subConfig);
    return updateContSubConfigsLocked(propIdAreaId, newConfig);
}

VhalResult<void> SubscriptionManager::removeContinuousSubscriberLocked(
        const ClientIdType& clientId, const PropIdAreaId& propIdAreaId) {
    // Make a copy so that we don't modify 'mContSubConfigsByPropIdArea' on failure cases.
    ContSubConfigs newConfig = mContSubConfigsByPropIdArea[propIdAreaId];
    newConfig.removeClient(clientId);
    return updateContSubConfigsLocked(propIdAreaId, newConfig);
}

VhalResult<void> SubscriptionManager::removeOnChangeSubscriberLocked(
        const PropIdAreaId& propIdAreaId) {
    if (mClientsByPropIdAreaId[propIdAreaId].size() > 1) {
        // After unsubscribing this client, there is still client subscribed, so do nothing.
        return {};
    }

    int32_t propId = propIdAreaId.propId;
    int32_t areaId = propIdAreaId.areaId;
    if (auto status = mVehicleHardware->unsubscribe(propId, areaId); status != StatusCode::OK) {
        return StatusError(status)
               << StringPrintf("failed unsubscribe for prop: %s, areaId: %" PRId32,
                               propIdToString(propId).c_str(), areaId);
    }
    return {};
}

VhalResult<void> SubscriptionManager::updateContSubConfigsLocked(const PropIdAreaId& propIdAreaId,
                                                                 const ContSubConfigs& newConfig) {
    const auto& oldConfig = mContSubConfigsByPropIdArea[propIdAreaId];
    float newRateHz = newConfig.getMaxSampleRateHz();
    float oldRateHz = oldConfig.getMaxSampleRateHz();
    float newResolution = newConfig.getMinRequiredResolution();
    float oldResolution = oldConfig.getMinRequiredResolution();
    if (newRateHz == oldRateHz && newResolution == oldResolution &&
        newConfig.isVurEnabled() == oldConfig.isVurEnabled()) {
        mContSubConfigsByPropIdArea[propIdAreaId] = newConfig;
        return {};
    }
    int32_t propId = propIdAreaId.propId;
    int32_t areaId = propIdAreaId.areaId;
    if (newRateHz != oldRateHz) {
        if (auto status = mVehicleHardware->updateSampleRate(propId, areaId, newRateHz);
            status != StatusCode::OK) {
            return StatusError(status)
                   << StringPrintf("failed to update sample rate for prop: %s, areaId: %" PRId32
                                   ", sample rate: %f HZ",
                                   propIdToString(propId).c_str(), areaId, newRateHz);
        }
    }
    if (newRateHz != 0) {
        if (auto status = mVehicleHardware->subscribe(newSubscribeOptions(
                    propId, areaId, newRateHz, newResolution, newConfig.isVurEnabled()));
            status != StatusCode::OK) {
            return StatusError(status) << StringPrintf(
                           "failed subscribe for prop: %s, areaId"
                           ": %" PRId32 ", sample rate: %f HZ",
                           propIdToString(propId).c_str(), areaId, newRateHz);
        }
    } else {
        if (auto status = mVehicleHardware->unsubscribe(propId, areaId); status != StatusCode::OK) {
            return StatusError(status) << StringPrintf(
                           "failed unsubscribe for prop: %s, areaId"
                           ": %" PRId32,
                           propIdToString(propId).c_str(), areaId);
        }
    }
    mContSubConfigsByPropIdArea[propIdAreaId] = newConfig;
    return {};
}

VhalResult<void> SubscriptionManager::subscribe(const std::shared_ptr<IVehicleCallback>& callback,
                                                const std::vector<SubscribeOptions>& options,
                                                bool isContinuousProperty) {
    std::scoped_lock<std::mutex> lockGuard(mLock);

    for (const auto& option : options) {
        float sampleRateHz = option.sampleRate;

        if (isContinuousProperty) {
            if (auto result = getIntervalNanos(sampleRateHz); !result.ok()) {
                return StatusError(StatusCode::INVALID_ARG) << result.error().message();
            }
            if (!checkResolution(option.resolution)) {
                return StatusError(StatusCode::INVALID_ARG) << StringPrintf(
                               "SubscribeOptions.resolution %f is not an integer power of 10",
                               option.resolution);
            }
        }

        if (option.areaIds.empty()) {
            ALOGE("area IDs to subscribe must not be empty");
            return StatusError(StatusCode::INVALID_ARG)
                   << "area IDs to subscribe must not be empty";
        }
    }

    ClientIdType clientId = callback->asBinder().get();

    for (const auto& option : options) {
        int32_t propId = option.propId;
        const std::vector<int32_t>& areaIds = option.areaIds;
        for (int32_t areaId : areaIds) {
            PropIdAreaId propIdAreaId = {
                    .propId = propId,
                    .areaId = areaId,
            };
            VhalResult<void> result;
            if (isContinuousProperty) {
                result = addContinuousSubscriberLocked(clientId, propIdAreaId, option.sampleRate,
                                                       option.resolution,
                                                       option.enableVariableUpdateRate);
            } else {
                result = addOnChangeSubscriberLocked(propIdAreaId);
            }

            if (!result.ok()) {
                return result;
            }

            mSubscribedPropsByClient[clientId].insert(propIdAreaId);
            mClientsByPropIdAreaId[propIdAreaId][clientId] = callback;
        }
    }
    return {};
}

VhalResult<void> SubscriptionManager::unsubscribePropIdAreaIdLocked(
        SubscriptionManager::ClientIdType clientId, const PropIdAreaId& propIdAreaId) {
    if (mContSubConfigsByPropIdArea.find(propIdAreaId) != mContSubConfigsByPropIdArea.end()) {
        // This is a subscribed continuous property.
        if (auto result = removeContinuousSubscriberLocked(clientId, propIdAreaId); !result.ok()) {
            return result;
        }
    } else {
        if (mClientsByPropIdAreaId.find(propIdAreaId) == mClientsByPropIdAreaId.end()) {
            ALOGW("Unsubscribe: The property: %s, areaId: %" PRId32
                  " was not previously subscribed, do nothing",
                  propIdToString(propIdAreaId.propId).c_str(), propIdAreaId.areaId);
            return {};
        }
        // This is an on-change property.
        if (auto result = removeOnChangeSubscriberLocked(propIdAreaId); !result.ok()) {
            return result;
        }
    }

    auto& clients = mClientsByPropIdAreaId[propIdAreaId];
    clients.erase(clientId);
    if (clients.empty()) {
        mClientsByPropIdAreaId.erase(propIdAreaId);
        mContSubConfigsByPropIdArea.erase(propIdAreaId);
    }
    return {};
}

VhalResult<void> SubscriptionManager::unsubscribe(SubscriptionManager::ClientIdType clientId,
                                                  const std::vector<int32_t>& propIds) {
    std::scoped_lock<std::mutex> lockGuard(mLock);

    if (mSubscribedPropsByClient.find(clientId) == mSubscribedPropsByClient.end()) {
        ALOGW("No property was subscribed for the callback, unsubscribe does nothing");
        return {};
    }

    std::vector<PropIdAreaId> propIdAreaIdsToUnsubscribe;
    std::unordered_set<int32_t> propIdSet;
    for (int32_t propId : propIds) {
        propIdSet.insert(propId);
    }
    auto& subscribedPropIdsAreaIds = mSubscribedPropsByClient[clientId];
    for (const auto& propIdAreaId : subscribedPropIdsAreaIds) {
        if (propIdSet.find(propIdAreaId.propId) != propIdSet.end()) {
            propIdAreaIdsToUnsubscribe.push_back(propIdAreaId);
        }
    }

    for (const auto& propIdAreaId : propIdAreaIdsToUnsubscribe) {
        if (auto result = unsubscribePropIdAreaIdLocked(clientId, propIdAreaId); !result.ok()) {
            return result;
        }
        subscribedPropIdsAreaIds.erase(propIdAreaId);
    }

    if (subscribedPropIdsAreaIds.empty()) {
        mSubscribedPropsByClient.erase(clientId);
    }
    return {};
}

VhalResult<void> SubscriptionManager::unsubscribe(SubscriptionManager::ClientIdType clientId) {
    std::scoped_lock<std::mutex> lockGuard(mLock);

    if (mSubscribedPropsByClient.find(clientId) == mSubscribedPropsByClient.end()) {
        ALOGW("No property was subscribed for this client, unsubscribe does nothing");
    } else {
        auto& propIdAreaIds = mSubscribedPropsByClient[clientId];
        for (auto const& propIdAreaId : propIdAreaIds) {
            if (auto result = unsubscribePropIdAreaIdLocked(clientId, propIdAreaId); !result.ok()) {
                return result;
            }
        }
        mSubscribedPropsByClient.erase(clientId);
    }

    if (mSupportedValueChangePropIdAreaIdsByClient.find(clientId) ==
        mSupportedValueChangePropIdAreaIdsByClient.end()) {
        ALOGW("No supported value change was subscribed for this client, unsubscribe does nothing");
    } else {
        const auto& propIdAreaIds = mSupportedValueChangePropIdAreaIdsByClient[clientId];
        if (auto result = unsubscribeSupportedValueChangeLocked(
                    clientId,
                    std::vector<PropIdAreaId>(propIdAreaIds.begin(), propIdAreaIds.end()));
            !result.ok()) {
            return result;
        }
    }
    return {};
}

VhalResult<void> SubscriptionManager::subscribeSupportedValueChange(
        const std::shared_ptr<IVehicleCallback>& callback,
        const std::vector<PropIdAreaId>& propIdAreaIds) {
    // Need to make sure this whole operation is guarded by a lock so that our internal state is
    // consistent with IVehicleHardware state.
    std::scoped_lock<std::mutex> lockGuard(mLock);

    ClientIdType clientId = callback->asBinder().get();

    // It is possible that some of the [propId, areaId]s are already subscribed, IVehicleHardware
    // will ignore them.
    if (auto status = mVehicleHardware->subscribeSupportedValueChange(propIdAreaIds);
        status != StatusCode::OK) {
        return StatusError(status)
               << fmt::format("failed to call subscribeSupportedValueChange for propIdAreaIds: {}",
                              propIdAreaIds);
    }
    for (const auto& propIdAreaId : propIdAreaIds) {
        mSupportedValueChangeClientsByPropIdAreaId[propIdAreaId][clientId] = callback;
        // mSupportedValueChangePropIdAreaIdsByClient[clientId] is a set so this will ignore
        // duplicate [propId, areaId].
        mSupportedValueChangePropIdAreaIdsByClient[clientId].insert(propIdAreaId);
    }
    return {};
}

VhalResult<void> SubscriptionManager::unsubscribeSupportedValueChange(
        SubscriptionManager::ClientIdType clientId,
        const std::vector<PropIdAreaId>& propIdAreaIds) {
    // Need to make sure this whole operation is guarded by a lock so that our internal state is
    // consistent with IVehicleHardware state.
    std::scoped_lock<std::mutex> lockGuard(mLock);

    return unsubscribeSupportedValueChangeLocked(clientId, propIdAreaIds);
}

VhalResult<void> SubscriptionManager::unsubscribeSupportedValueChangeLocked(
        SubscriptionManager::ClientIdType clientId,
        const std::vector<PropIdAreaId>& propIdAreaIds) {
    std::vector<PropIdAreaId> propIdAreaIdsToUnsubscribe;

    // Check which [propId, areaId] needs to be unsubscribed from the hardware.
    for (const auto& propIdAreaId : propIdAreaIds) {
        auto it = mSupportedValueChangeClientsByPropIdAreaId.find(propIdAreaId);
        if (it != mSupportedValueChangeClientsByPropIdAreaId.end()) {
            const auto& clients = it->second;
            if (clients.size() == 1 && clients.find(clientId) != clients.end()) {
                // This callback is the only client registered for [propId, areaId].
                // Unregister it should unregister the [propId, areaId].
                propIdAreaIdsToUnsubscribe.push_back(propIdAreaId);
            }
        }
    }

    // Send the unsubscribe request.
    if (!propIdAreaIdsToUnsubscribe.empty()) {
        if (auto status =
                    mVehicleHardware->unsubscribeSupportedValueChange(propIdAreaIdsToUnsubscribe);
            status != StatusCode::OK) {
            return StatusError(status) << fmt::format(
                           "failed to call unsubscribeSupportedValueChange for "
                           "propIdAreaIds: {}",
                           propIdAreaIdsToUnsubscribe);
        }
    }

    // Remove internal book-keeping.
    for (const auto& propIdAreaId : propIdAreaIds) {
        if (mSupportedValueChangeClientsByPropIdAreaId.find(propIdAreaId) !=
            mSupportedValueChangeClientsByPropIdAreaId.end()) {
            mSupportedValueChangeClientsByPropIdAreaId[propIdAreaId].erase(clientId);
        }
        if (mSupportedValueChangeClientsByPropIdAreaId[propIdAreaId].empty()) {
            mSupportedValueChangeClientsByPropIdAreaId.erase(propIdAreaId);
        }
        mSupportedValueChangePropIdAreaIdsByClient[clientId].erase(propIdAreaId);
        if (mSupportedValueChangePropIdAreaIdsByClient[clientId].empty()) {
            mSupportedValueChangePropIdAreaIdsByClient.erase(clientId);
        }
    }
    return {};
}

bool SubscriptionManager::isValueUpdatedLocked(const std::shared_ptr<IVehicleCallback>& callback,
                                               const VehiclePropValue& value) {
    const auto& it = mContSubValuesByCallback[callback].find(value);
    if (it == mContSubValuesByCallback[callback].end()) {
        mContSubValuesByCallback[callback].insert(value);
        return true;
    }

    if (it->timestamp > value.timestamp) {
        ALOGE("The updated property value: %s is outdated, ignored", value.toString().c_str());
        return false;
    }

    if (it->value == value.value && it->status == value.status) {
        // Even though the property value is the same, we need to store the new property event to
        // update the timestamp.
        mContSubValuesByCallback[callback].insert(value);
        ALOGD("The updated property value for propId: %" PRId32 ", areaId: %" PRId32
              " has the "
              "same value and status, ignored if VUR is enabled",
              it->prop, it->areaId);
        return false;
    }

    mContSubValuesByCallback[callback].insert(value);
    return true;
}

std::unordered_map<std::shared_ptr<IVehicleCallback>, std::vector<VehiclePropValue>>
SubscriptionManager::getSubscribedClients(std::vector<VehiclePropValue>&& updatedValues) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    std::unordered_map<std::shared_ptr<IVehicleCallback>, std::vector<VehiclePropValue>> clients;

    for (auto& value : updatedValues) {
        PropIdAreaId propIdAreaId{
                .propId = value.prop,
                .areaId = value.areaId,
        };
        if (mClientsByPropIdAreaId.find(propIdAreaId) == mClientsByPropIdAreaId.end()) {
            continue;
        }

        for (const auto& [client, callback] : mClientsByPropIdAreaId[propIdAreaId]) {
            // if propId is on-change, propIdAreaId will not exist in mContSubConfigsByPropIdArea,
            // returning an empty ContSubConfigs value for subConfigs i.e. with resolution = 0 and
            // enableVur = false.
            auto& subConfigs = mContSubConfigsByPropIdArea[propIdAreaId];
            // Clients must be sent different VehiclePropValues with different levels of granularity
            // as requested by the client using resolution.
            VehiclePropValue newValue = value;
            sanitizeByResolution(&(newValue.value), subConfigs.getResolutionForClient(client));
            // If client wants VUR (and VUR is supported as checked in DefaultVehicleHal), it is
            // possible that VUR is not enabled in IVehicleHardware because another client does not
            // enable VUR. We will implement VUR filtering here for the client that enables it.
            if (subConfigs.isVurEnabledForClient(client) && !subConfigs.isVurEnabled()) {
                if (isValueUpdatedLocked(callback, newValue)) {
                    clients[callback].push_back(newValue);
                }
            } else {
                clients[callback].push_back(newValue);
            }
        }
    }
    return clients;
}

std::unordered_map<std::shared_ptr<IVehicleCallback>, std::vector<VehiclePropError>>
SubscriptionManager::getSubscribedClientsForErrorEvents(
        const std::vector<SetValueErrorEvent>& errorEvents) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    std::unordered_map<std::shared_ptr<IVehicleCallback>, std::vector<VehiclePropError>> clients;

    for (const auto& errorEvent : errorEvents) {
        PropIdAreaId propIdAreaId{
                .propId = errorEvent.propId,
                .areaId = errorEvent.areaId,
        };
        if (mClientsByPropIdAreaId.find(propIdAreaId) == mClientsByPropIdAreaId.end()) {
            continue;
        }

        for (const auto& [_, client] : mClientsByPropIdAreaId[propIdAreaId]) {
            clients[client].push_back({
                    .propId = errorEvent.propId,
                    .areaId = errorEvent.areaId,
                    .errorCode = errorEvent.errorCode,
            });
        }
    }
    return clients;
}

std::unordered_map<std::shared_ptr<IVehicleCallback>, std::vector<PropIdAreaId>>
SubscriptionManager::getSubscribedClientsForSupportedValueChange(
        const std::vector<PropIdAreaId>& propIdAreaIds) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    std::unordered_map<std::shared_ptr<IVehicleCallback>, std::vector<PropIdAreaId>>
            propIdAreaIdsByClient;

    for (const auto& propIdAreaId : propIdAreaIds) {
        const auto clientIter = mSupportedValueChangeClientsByPropIdAreaId.find(propIdAreaId);
        if (clientIter == mSupportedValueChangeClientsByPropIdAreaId.end()) {
            continue;
        }
        for (const auto& [_, client] : clientIter->second) {
            propIdAreaIdsByClient[client].push_back(propIdAreaId);
        }
    }
    return propIdAreaIdsByClient;
}

bool SubscriptionManager::isEmpty() {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    return mSubscribedPropsByClient.empty() && mClientsByPropIdAreaId.empty() &&
           mSupportedValueChangeClientsByPropIdAreaId.empty() &&
           mSupportedValueChangePropIdAreaIdsByClient.empty();
}

size_t SubscriptionManager::countPropertyChangeClients() {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    return mSubscribedPropsByClient.size();
}

size_t SubscriptionManager::countSupportedValueChangeClients() {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    return mSupportedValueChangePropIdAreaIdsByClient.size();
}

}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android
