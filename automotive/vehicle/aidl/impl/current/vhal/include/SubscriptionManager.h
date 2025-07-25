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

#ifndef android_hardware_automotive_vehicle_aidl_impl_vhal_include_SubscriptionManager_H_
#define android_hardware_automotive_vehicle_aidl_impl_vhal_include_SubscriptionManager_H_

#include <IVehicleHardware.h>
#include <VehicleHalTypes.h>
#include <VehicleUtils.h>

#include <aidl/android/hardware/automotive/vehicle/IVehicleCallback.h>
#include <android-base/result.h>
#include <android-base/thread_annotations.h>

#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {

// A structure to represent subscription config for one subscription client.
struct SubConfig {
    float sampleRateHz;
    float resolution;
    bool enableVur;
};

// A class to represent all the subscription configs for a continuous [propId, areaId].
class ContSubConfigs final {
  public:
    using ClientIdType = const AIBinder*;

    void addClient(const ClientIdType& clientId, const SubConfig& subConfig);
    void removeClient(const ClientIdType& clientId);
    float getMaxSampleRateHz() const;
    float getMinRequiredResolution() const;
    bool isVurEnabled() const;
    bool isVurEnabledForClient(const ClientIdType& clientId) const;
    float getResolutionForClient(const ClientIdType& clientId) const;

  private:
    float mMaxSampleRateHz = 0.;
    // Baseline for resolution is maximum possible float. We want to sanitize to the highest
    // requested resolution, which is the smallest float value for resolution.
    float mMinRequiredResolution = std::numeric_limits<float>::max();
    bool mEnableVur;
    std::unordered_map<ClientIdType, SubConfig> mConfigByClient;

    void refreshCombinedConfig();
};

// A thread-safe subscription manager that manages all VHAL subscriptions.
class SubscriptionManager final {
  public:
    using ClientIdType = const AIBinder*;
    using CallbackType =
            std::shared_ptr<aidl::android::hardware::automotive::vehicle::IVehicleCallback>;
    using VehiclePropValue = aidl::android::hardware::automotive::vehicle::VehiclePropValue;

    explicit SubscriptionManager(IVehicleHardware* vehicleHardware);
    ~SubscriptionManager();

    // Subscribes to properties according to {@code SubscribeOptions}. Note that all option must
    // contain non-empty areaIds field, which contains all area IDs to subscribe. As a result,
    // the options here is different from the options passed from VHAL client.
    // Returns error if any of the subscribe options is not valid or one of the properties failed
    // to subscribe. Part of the properties maybe be subscribed successfully if this function
    // returns error. Caller is safe to retry since subscribing to an already subscribed property
    // is okay.
    // Returns ok if all the options are parsed correctly and all the properties are subscribed.
    VhalResult<void> subscribe(
            const CallbackType& callback,
            const std::vector<aidl::android::hardware::automotive::vehicle::SubscribeOptions>&
                    options,
            bool isContinuousProperty);

    // Unsubscribes from the properties for the client.
    // Returns error if one of the property failed to unsubscribe. Caller is safe to retry since
    // unsubscribing to an already unsubscribed property is okay (it would be ignored).
    // Returns ok if all the requested properties for the client are unsubscribed.
    VhalResult<void> unsubscribe(ClientIdType client, const std::vector<int32_t>& propIds);

    // Unsubscribes from all the properties for the client.
    // Returns error one of the subscribed properties for the client failed to unsubscribe.
    // Caller is safe to retry.
    // Returns ok if all the properties for the client are unsubscribed.
    VhalResult<void> unsubscribe(ClientIdType client);

    // For a list of updated properties, returns a map that maps clients subscribing to
    // the updated properties to a list of updated values. This would only return on-change property
    // clients that should be informed for the given updated values.
    std::unordered_map<CallbackType, std::vector<VehiclePropValue>> getSubscribedClients(
            std::vector<VehiclePropValue>&& updatedValues);

    // For a list of set property error events, returns a map that maps clients subscribing to the
    // properties to a list of errors for each client.
    std::unordered_map<CallbackType,
                       std::vector<aidl::android::hardware::automotive::vehicle::VehiclePropError>>
    getSubscribedClientsForErrorEvents(const std::vector<SetValueErrorEvent>& errorEvents);

    // For a list of [propId, areaId]s that has updated supported value, returns a map that maps
    // subscribing clients to updated [propId, areaId]s.
    std::unordered_map<CallbackType, std::vector<PropIdAreaId>>
    getSubscribedClientsForSupportedValueChange(const std::vector<PropIdAreaId>& propIdAreaIds);

    // Subscribes to supported values change.
    VhalResult<void> subscribeSupportedValueChange(const CallbackType& callback,
                                                   const std::vector<PropIdAreaId>& propIdAreaIds);

    // Unsubscribes to supported values change.
    VhalResult<void> unsubscribeSupportedValueChange(
            ClientIdType client, const std::vector<PropIdAreaId>& propIdAreaIds);

    // Returns the number of subscribed property change clients.
    size_t countPropertyChangeClients();

    // Returns the number of subscribed supported value change clients.
    size_t countSupportedValueChangeClients();

    // Checks whether the sample rate is valid.
    static bool checkSampleRateHz(float sampleRateHz);

    // Checks whether the resolution is valid.
    static bool checkResolution(float resolution);

  private:
    // Friend class for testing.
    friend class DefaultVehicleHalTest;
    friend class SubscriptionManagerTest;

    IVehicleHardware* mVehicleHardware;

    struct VehiclePropValueHashPropIdAreaId {
        inline size_t operator()(const VehiclePropValue& vehiclePropValue) const {
            size_t res = 0;
            hashCombine(res, vehiclePropValue.prop);
            hashCombine(res, vehiclePropValue.areaId);
            return res;
        }
    };

    struct VehiclePropValueEqualPropIdAreaId {
        inline bool operator()(const VehiclePropValue& left, const VehiclePropValue& right) const {
            return left.prop == right.prop && left.areaId == right.areaId;
        }
    };

    mutable std::mutex mLock;
    std::unordered_map<PropIdAreaId, std::unordered_map<ClientIdType, CallbackType>,
                       PropIdAreaIdHash>
            mClientsByPropIdAreaId GUARDED_BY(mLock);
    std::unordered_map<ClientIdType, std::unordered_set<PropIdAreaId, PropIdAreaIdHash>>
            mSubscribedPropsByClient GUARDED_BY(mLock);
    std::unordered_map<PropIdAreaId, ContSubConfigs, PropIdAreaIdHash> mContSubConfigsByPropIdArea
            GUARDED_BY(mLock);
    std::unordered_map<CallbackType,
                       std::unordered_set<VehiclePropValue, VehiclePropValueHashPropIdAreaId,
                                          VehiclePropValueEqualPropIdAreaId>>
            mContSubValuesByCallback GUARDED_BY(mLock);
    std::unordered_map<PropIdAreaId, std::unordered_map<ClientIdType, CallbackType>,
                       PropIdAreaIdHash>
            mSupportedValueChangeClientsByPropIdAreaId GUARDED_BY(mLock);
    std::unordered_map<ClientIdType, std::unordered_set<PropIdAreaId, PropIdAreaIdHash>>
            mSupportedValueChangePropIdAreaIdsByClient GUARDED_BY(mLock);

    VhalResult<void> addContinuousSubscriberLocked(const ClientIdType& clientId,
                                                   const PropIdAreaId& propIdAreaId,
                                                   float sampleRateHz, float resolution,
                                                   bool enableVur) REQUIRES(mLock);
    VhalResult<void> addOnChangeSubscriberLocked(const PropIdAreaId& propIdAreaId) REQUIRES(mLock);
    // Removes the subscription client for the continuous [propId, areaId].
    VhalResult<void> removeContinuousSubscriberLocked(const ClientIdType& clientId,
                                                      const PropIdAreaId& propIdAreaId)
            REQUIRES(mLock);
    // Removes one subscription client for the on-change [propId, areaId].
    VhalResult<void> removeOnChangeSubscriberLocked(const PropIdAreaId& propIdAreaId)
            REQUIRES(mLock);

    VhalResult<void> updateContSubConfigsLocked(const PropIdAreaId& PropIdAreaId,
                                                const ContSubConfigs& newConfig) REQUIRES(mLock);

    VhalResult<void> unsubscribePropIdAreaIdLocked(SubscriptionManager::ClientIdType clientId,
                                                   const PropIdAreaId& propIdAreaId)
            REQUIRES(mLock);
    VhalResult<void> unsubscribeSupportedValueChangeLocked(
            SubscriptionManager::ClientIdType clientId,
            const std::vector<PropIdAreaId>& propIdAreaIds) REQUIRES(mLock);

    // Checks whether the manager is empty. For testing purpose.
    bool isEmpty();

    bool isValueUpdatedLocked(const CallbackType& callback, const VehiclePropValue& value)
            REQUIRES(mLock);

    // Get the interval in nanoseconds accroding to sample rate.
    static android::base::Result<int64_t> getIntervalNanos(float sampleRateHz);
};

}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android

#endif  // android_hardware_automotive_vehicle_aidl_impl_vhal_include_SubscriptionManager_H_
