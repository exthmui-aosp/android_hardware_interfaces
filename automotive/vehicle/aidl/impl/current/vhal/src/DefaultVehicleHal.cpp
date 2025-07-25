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

#define LOG_TAG "DefaultVehicleHal"
#define ATRACE_TAG ATRACE_TAG_HAL

#include <DefaultVehicleHal.h>

#include <LargeParcelableBase.h>
#include <VehicleHalTypes.h>
#include <VehicleUtils.h>
#include <VersionForVehicleProperty.h>

#include <android-base/logging.h>
#include <android-base/result.h>
#include <android-base/stringprintf.h>
#include <android/binder_ibinder.h>
#include <private/android_filesystem_config.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>
#include <utils/Trace.h>

#include <inttypes.h>
#include <chrono>
#include <set>
#include <unordered_set>

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {

namespace {

using ::aidl::android::hardware::automotive::vehicle::GetValueRequest;
using ::aidl::android::hardware::automotive::vehicle::GetValueRequests;
using ::aidl::android::hardware::automotive::vehicle::GetValueResult;
using ::aidl::android::hardware::automotive::vehicle::GetValueResults;
using ::aidl::android::hardware::automotive::vehicle::HasSupportedValueInfo;
using ::aidl::android::hardware::automotive::vehicle::IVehicleCallback;
using ::aidl::android::hardware::automotive::vehicle::MinMaxSupportedValueResult;
using ::aidl::android::hardware::automotive::vehicle::MinMaxSupportedValueResults;
using ::aidl::android::hardware::automotive::vehicle::SetValueRequest;
using ::aidl::android::hardware::automotive::vehicle::SetValueRequests;
using ::aidl::android::hardware::automotive::vehicle::SetValueResult;
using ::aidl::android::hardware::automotive::vehicle::SetValueResults;
using ::aidl::android::hardware::automotive::vehicle::StatusCode;
using ::aidl::android::hardware::automotive::vehicle::SubscribeOptions;
using ::aidl::android::hardware::automotive::vehicle::SupportedValuesListResult;
using ::aidl::android::hardware::automotive::vehicle::SupportedValuesListResults;
using ::aidl::android::hardware::automotive::vehicle::VehicleAreaConfig;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropConfig;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropConfigs;
using ::aidl::android::hardware::automotive::vehicle::VehicleProperty;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyAccess;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyChangeMode;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyStatus;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropValue;
using ::aidl::android::hardware::automotive::vehicle::VersionForVehicleProperty;
using ::android::automotive::car_binder_lib::LargeParcelableBase;
using ::android::base::Error;
using ::android::base::expected;
using ::android::base::Result;
using ::android::base::StringPrintf;

using ::ndk::ScopedAIBinder_DeathRecipient;
using ::ndk::ScopedAStatus;
using ::ndk::ScopedFileDescriptor;

using VhalPropIdAreaId = ::aidl::android::hardware::automotive::vehicle::PropIdAreaId;

std::string toString(const std::unordered_set<int64_t>& values) {
    std::string str = "";
    for (auto it = values.begin(); it != values.end(); it++) {
        str += std::to_string(*it);
        if (std::next(it, 1) != values.end()) {
            str += ", ";
        }
    }
    return str;
}

float getDefaultSampleRateHz(float sampleRateHz, float minSampleRateHz, float maxSampleRateHz) {
    if (sampleRateHz < minSampleRateHz) {
        return minSampleRateHz;
    }
    if (sampleRateHz > maxSampleRateHz) {
        return maxSampleRateHz;
    }
    return sampleRateHz;
}

class SCOPED_CAPABILITY SharedScopedLockAssertion {
  public:
    SharedScopedLockAssertion(std::shared_timed_mutex& mutex) ACQUIRE_SHARED(mutex) {}
    ~SharedScopedLockAssertion() RELEASE() {}
};

class SCOPED_CAPABILITY UniqueScopedLockAssertion {
  public:
    UniqueScopedLockAssertion(std::shared_timed_mutex& mutex) ACQUIRE(mutex) {}
    ~UniqueScopedLockAssertion() RELEASE() {}
};

}  // namespace

DefaultVehicleHal::DefaultVehicleHal(std::unique_ptr<IVehicleHardware> vehicleHardware)
    : DefaultVehicleHal(std::move(vehicleHardware), /* testInterfaceVersion= */ 0) {};

DefaultVehicleHal::DefaultVehicleHal(std::unique_ptr<IVehicleHardware> vehicleHardware,
                                     int32_t testInterfaceVersion)
    : mVehicleHardware(std::move(vehicleHardware)),
      mPendingRequestPool(std::make_shared<PendingRequestPool>(TIMEOUT_IN_NANO)),
      mTestInterfaceVersion(testInterfaceVersion) {
    ALOGD("DefaultVehicleHal init");
    IVehicleHardware* vehicleHardwarePtr = mVehicleHardware.get();
    mSubscriptionManager = std::make_shared<SubscriptionManager>(vehicleHardwarePtr);
    mEventBatchingWindow = mVehicleHardware->getPropertyOnChangeEventBatchingWindow();
    if (mEventBatchingWindow != std::chrono::nanoseconds(0)) {
        mBatchedEventQueue = std::make_shared<ConcurrentQueue<VehiclePropValue>>();
        mPropertyChangeEventsBatchingConsumer =
                std::make_shared<BatchingConsumer<VehiclePropValue>>();
        mPropertyChangeEventsBatchingConsumer->run(
                mBatchedEventQueue.get(), mEventBatchingWindow,
                [this](std::vector<VehiclePropValue> batchedEvents) {
                    handleBatchedPropertyEvents(std::move(batchedEvents));
                });
    }

    std::weak_ptr<ConcurrentQueue<VehiclePropValue>> batchedEventQueueCopy = mBatchedEventQueue;
    std::chrono::nanoseconds eventBatchingWindow = mEventBatchingWindow;
    std::weak_ptr<SubscriptionManager> subscriptionManagerCopy = mSubscriptionManager;
    mVehicleHardware->registerOnPropertyChangeEvent(
            std::make_unique<IVehicleHardware::PropertyChangeCallback>(
                    [subscriptionManagerCopy, batchedEventQueueCopy,
                     eventBatchingWindow](std::vector<VehiclePropValue> updatedValues) {
                        if (eventBatchingWindow != std::chrono::nanoseconds(0)) {
                            batchPropertyChangeEvent(batchedEventQueueCopy,
                                                     std::move(updatedValues));
                        } else {
                            onPropertyChangeEvent(subscriptionManagerCopy,
                                                  std::move(updatedValues));
                        }
                    }));
    mVehicleHardware->registerOnPropertySetErrorEvent(
            std::make_unique<IVehicleHardware::PropertySetErrorCallback>(
                    [subscriptionManagerCopy](std::vector<SetValueErrorEvent> errorEvents) {
                        onPropertySetErrorEvent(subscriptionManagerCopy, errorEvents);
                    }));
    mVehicleHardware->registerSupportedValueChangeCallback(
            std::make_unique<IVehicleHardware::SupportedValueChangeCallback>(
                    [subscriptionManagerCopy](std::vector<PropIdAreaId> propIdAreaIds) {
                        onSupportedValueChange(subscriptionManagerCopy, propIdAreaIds);
                    }));

    // Register heartbeat event.
    mRecurrentAction = std::make_shared<std::function<void()>>(
            [vehicleHardwarePtr, subscriptionManagerCopy]() {
                checkHealth(vehicleHardwarePtr, subscriptionManagerCopy);
            });
    mRecurrentTimer.registerTimerCallback(HEART_BEAT_INTERVAL_IN_NANO, mRecurrentAction);

    mBinderLifecycleHandler = std::make_unique<BinderLifecycleHandler>();
    mOnBinderDiedUnlinkedHandlerThread = std::thread([this] { onBinderDiedUnlinkedHandler(); });
    mDeathRecipient = ScopedAIBinder_DeathRecipient(
            AIBinder_DeathRecipient_new(&DefaultVehicleHal::onBinderDied));
    AIBinder_DeathRecipient_setOnUnlinked(mDeathRecipient.get(),
                                          &DefaultVehicleHal::onBinderUnlinked);
}

DefaultVehicleHal::~DefaultVehicleHal() {
    // Delete the deathRecipient so that onBinderDied would not be called to reference 'this'.
    mDeathRecipient = ScopedAIBinder_DeathRecipient();
    mBinderEvents.deactivate();
    if (mOnBinderDiedUnlinkedHandlerThread.joinable()) {
        mOnBinderDiedUnlinkedHandlerThread.join();
    }
    // mRecurrentAction uses pointer to mVehicleHardware, so it has to be unregistered before
    // mVehicleHardware.
    mRecurrentTimer.unregisterTimerCallback(mRecurrentAction);

    if (mBatchedEventQueue) {
        // mPropertyChangeEventsBatchingConsumer uses mSubscriptionManager and mBatchedEventQueue.
        mBatchedEventQueue->deactivate();
        mPropertyChangeEventsBatchingConsumer->requestStop();
        mPropertyChangeEventsBatchingConsumer->waitStopped();
        mPropertyChangeEventsBatchingConsumer.reset();
        mBatchedEventQueue.reset();
    }

    // mSubscriptionManager uses pointer to mVehicleHardware, so it has to be destroyed before
    // mVehicleHardware.
    mSubscriptionManager.reset();
    mVehicleHardware.reset();
}

void DefaultVehicleHal::batchPropertyChangeEvent(
        const std::weak_ptr<ConcurrentQueue<VehiclePropValue>>& batchedEventQueue,
        std::vector<VehiclePropValue>&& updatedValues) {
    auto batchedEventQueueStrong = batchedEventQueue.lock();
    if (batchedEventQueueStrong == nullptr) {
        ALOGW("%s: the batched property events queue is destroyed, DefaultVehicleHal is ending",
              __func__);
        return;
    }
    batchedEventQueueStrong->push(std::move(updatedValues));
}

void DefaultVehicleHal::handleBatchedPropertyEvents(std::vector<VehiclePropValue>&& batchedEvents) {
    onPropertyChangeEvent(mSubscriptionManager, std::move(batchedEvents));
}

void DefaultVehicleHal::onPropertyChangeEvent(
        const std::weak_ptr<SubscriptionManager>& subscriptionManager,
        std::vector<VehiclePropValue>&& updatedValues) {
    ATRACE_CALL();
    auto manager = subscriptionManager.lock();
    if (manager == nullptr) {
        ALOGW("%s: the SubscriptionManager is destroyed, DefaultVehicleHal is ending", __func__);
        return;
    }
    auto updatedValuesByClients = manager->getSubscribedClients(std::move(updatedValues));
    for (auto& [callback, values] : updatedValuesByClients) {
        SubscriptionClient::sendUpdatedValues(callback, std::move(values));
    }
}

void DefaultVehicleHal::onPropertySetErrorEvent(
        const std::weak_ptr<SubscriptionManager>& subscriptionManager,
        const std::vector<SetValueErrorEvent>& errorEvents) {
    auto manager = subscriptionManager.lock();
    if (manager == nullptr) {
        ALOGW("%s: the SubscriptionManager is destroyed, DefaultVehicleHal is ending", __func__);
        return;
    }
    auto vehiclePropErrorsByClient = manager->getSubscribedClientsForErrorEvents(errorEvents);
    for (auto& [callback, vehiclePropErrors] : vehiclePropErrorsByClient) {
        SubscriptionClient::sendPropertySetErrors(callback, std::move(vehiclePropErrors));
    }
}

void DefaultVehicleHal::onSupportedValueChange(
        const std::weak_ptr<SubscriptionManager>& subscriptionManager,
        const std::vector<PropIdAreaId>& propIdAreaIds) {
    auto manager = subscriptionManager.lock();
    if (manager == nullptr) {
        ALOGW("%s: the SubscriptionManager is destroyed, DefaultVehicleHal is ending", __func__);
        return;
    }
    auto updatedPropIdAreaIdsByClient =
            manager->getSubscribedClientsForSupportedValueChange(propIdAreaIds);
    for (auto& [callback, updatedPropIdAreaIds] : updatedPropIdAreaIdsByClient) {
        SubscriptionClient::sendSupportedValueChangeEvents(callback, updatedPropIdAreaIds);
    }
}

template <class T>
std::shared_ptr<T> DefaultVehicleHal::getOrCreateClient(
        std::unordered_map<const AIBinder*, std::shared_ptr<T>>* clients,
        const CallbackType& callback, std::shared_ptr<PendingRequestPool> pendingRequestPool) {
    const AIBinder* clientId = callback->asBinder().get();
    if (clients->find(clientId) == clients->end()) {
        (*clients)[clientId] = std::make_shared<T>(pendingRequestPool, callback);
    }
    return (*clients)[clientId];
}

bool DefaultVehicleHal::monitorBinderLifeCycleLocked(const AIBinder* clientId) {
    OnBinderDiedContext* contextPtr = nullptr;
    if (mOnBinderDiedContexts.find(clientId) != mOnBinderDiedContexts.end()) {
        return mBinderLifecycleHandler->isAlive(clientId);
    } else {
        std::unique_ptr<OnBinderDiedContext> context = std::make_unique<OnBinderDiedContext>(
                OnBinderDiedContext{.vhal = this, .clientId = clientId});
        // We know context must be alive when we use contextPtr because context would only
        // be removed in OnBinderUnlinked, which must be called after OnBinderDied.
        contextPtr = context.get();
        // Insert into a map to keep the context object alive.
        mOnBinderDiedContexts[clientId] = std::move(context);
    }

    // If this function fails, onBinderUnlinked would be called to remove the added context.
    binder_status_t status = mBinderLifecycleHandler->linkToDeath(
            const_cast<AIBinder*>(clientId), mDeathRecipient.get(), static_cast<void*>(contextPtr));
    if (status == STATUS_OK) {
        return true;
    }
    ALOGE("failed to call linkToDeath on client binder, client may already died, status: %d",
          static_cast<int>(status));
    return false;
}

void DefaultVehicleHal::onBinderDied(void* cookie) {
    OnBinderDiedContext* context = reinterpret_cast<OnBinderDiedContext*>(cookie);
    // To be handled in mOnBinderDiedUnlinkedHandlerThread. We cannot handle the event in the same
    // thread because we might be holding the mLock the handler requires.
    context->vhal->mBinderEvents.push(
            BinderDiedUnlinkedEvent{/*forOnBinderDied=*/true, context->clientId});
}

void DefaultVehicleHal::onBinderDiedWithContext(const AIBinder* clientId) {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    ALOGD("binder died, client ID: %p", clientId);
    mSetValuesClients.erase(clientId);
    mGetValuesClients.erase(clientId);
    mSubscriptionManager->unsubscribe(clientId);
}

void DefaultVehicleHal::onBinderUnlinked(void* cookie) {
    OnBinderDiedContext* context = reinterpret_cast<OnBinderDiedContext*>(cookie);
    // To be handled in mOnBinderDiedUnlinkedHandlerThread. We cannot handle the event in the same
    // thread because we might be holding the mLock the handler requires.
    context->vhal->mBinderEvents.push(
            BinderDiedUnlinkedEvent{/*forOnBinderDied=*/false, context->clientId});
}

void DefaultVehicleHal::onBinderUnlinkedWithContext(const AIBinder* clientId) {
    ALOGD("binder unlinked");
    std::scoped_lock<std::mutex> lockGuard(mLock);
    // Delete the context associated with this cookie.
    mOnBinderDiedContexts.erase(clientId);
}

void DefaultVehicleHal::onBinderDiedUnlinkedHandler() {
    while (mBinderEvents.waitForItems()) {
        for (BinderDiedUnlinkedEvent& event : mBinderEvents.flush()) {
            if (event.forOnBinderDied) {
                onBinderDiedWithContext(event.clientId);
            } else {
                onBinderUnlinkedWithContext(event.clientId);
            }
        }
    }
}

template std::shared_ptr<DefaultVehicleHal::GetValuesClient>
DefaultVehicleHal::getOrCreateClient<DefaultVehicleHal::GetValuesClient>(
        std::unordered_map<const AIBinder*, std::shared_ptr<GetValuesClient>>* clients,
        const CallbackType& callback, std::shared_ptr<PendingRequestPool> pendingRequestPool);
template std::shared_ptr<DefaultVehicleHal::SetValuesClient>
DefaultVehicleHal::getOrCreateClient<DefaultVehicleHal::SetValuesClient>(
        std::unordered_map<const AIBinder*, std::shared_ptr<SetValuesClient>>* clients,
        const CallbackType& callback, std::shared_ptr<PendingRequestPool> pendingRequestPool);

void DefaultVehicleHal::setTimeout(int64_t timeoutInNano) {
    mPendingRequestPool = std::make_unique<PendingRequestPool>(timeoutInNano);
}

int32_t DefaultVehicleHal::getVhalInterfaceVersion() const {
    if (mTestInterfaceVersion != 0) {
        return mTestInterfaceVersion;
    }
    int32_t myVersion = 0;
    // getInterfaceVersion is in-reality a const method.
    const_cast<DefaultVehicleHal*>(this)->getInterfaceVersion(&myVersion);
    return myVersion;
}

bool DefaultVehicleHal::isConfigSupportedForCurrentVhalVersion(
        const VehiclePropConfig& config) const {
    int32_t myVersion = getVhalInterfaceVersion();
    if (!isSystemProp(config.prop)) {
        return true;
    }
    VehicleProperty property = static_cast<VehicleProperty>(config.prop);
    std::string propertyName = aidl::android::hardware::automotive::vehicle::toString(property);
    auto it = VersionForVehicleProperty.find(property);
    if (it == VersionForVehicleProperty.end()) {
        ALOGE("The property: %s is not a supported system property, ignore", propertyName.c_str());
        return false;
    }
    int requiredVersion = it->second;
    if (myVersion < requiredVersion) {
        ALOGE("The property: %s is not supported for current client VHAL version, "
              "require %d, current version: %d, ignore",
              propertyName.c_str(), requiredVersion, myVersion);
        return false;
    }
    return true;
}

bool DefaultVehicleHal::getAllPropConfigsFromHardwareLocked() const {
    ALOGD("Get all property configs from hardware");
    auto configs = mVehicleHardware->getAllPropertyConfigs();
    std::vector<VehiclePropConfig> filteredConfigs;
    for (const auto& config : configs) {
        if (isConfigSupportedForCurrentVhalVersion(config)) {
            filteredConfigs.push_back(std::move(config));
        }
    }

    {
        std::unique_lock<std::shared_timed_mutex> configWriteLock(mConfigLock);
        UniqueScopedLockAssertion lockAssertion(mConfigLock);

        for (auto& config : filteredConfigs) {
            mConfigsByPropId[config.prop] = config;
        }
        VehiclePropConfigs vehiclePropConfigs;
        vehiclePropConfigs.payloads = std::move(filteredConfigs);
        auto result = LargeParcelableBase::parcelableToStableLargeParcelable(vehiclePropConfigs);
        if (!result.ok()) {
            ALOGE("failed to convert configs to shared memory file, error: %s, code: %d",
                  result.error().message().c_str(), static_cast<int>(result.error().code()));
            mConfigFile = nullptr;
            return false;
        }

        if (result.value() != nullptr) {
            mConfigFile = std::move(result.value());
        }
    }

    mConfigInit = true;
    return true;
}

void DefaultVehicleHal::getConfigsByPropId(
        std::function<void(const std::unordered_map<int32_t, VehiclePropConfig>&)> callback) const {
    if (!mConfigInit) {
        CHECK(getAllPropConfigsFromHardwareLocked())
                << "Failed to get property configs from hardware";
    }

    std::shared_lock<std::shared_timed_mutex> configReadLock(mConfigLock);
    SharedScopedLockAssertion lockAssertion(mConfigLock);

    callback(mConfigsByPropId);
}

ScopedAStatus DefaultVehicleHal::getAllPropConfigs(VehiclePropConfigs* output) {
    if (!mConfigInit) {
        CHECK(getAllPropConfigsFromHardwareLocked())
                << "Failed to get property configs from hardware";
    }

    std::shared_lock<std::shared_timed_mutex> configReadLock(mConfigLock);
    SharedScopedLockAssertion lockAssertion(mConfigLock);

    if (mConfigFile != nullptr) {
        output->payloads.clear();
        output->sharedMemoryFd.set(dup(mConfigFile->get()));
        return ScopedAStatus::ok();
    }

    output->payloads.reserve(mConfigsByPropId.size());
    for (const auto& [_, config] : mConfigsByPropId) {
        output->payloads.push_back(config);
    }
    return ScopedAStatus::ok();
}

Result<VehiclePropConfig> DefaultVehicleHal::getConfig(int32_t propId) const {
    Result<VehiclePropConfig> result;

    if (!mConfigInit) {
        std::optional<VehiclePropConfig> config = mVehicleHardware->getPropertyConfig(propId);
        if (!config.has_value()) {
            return Error() << "no config for property, ID: " << propId;
        }
        if (!isConfigSupportedForCurrentVhalVersion(config.value())) {
            return Error() << "property not supported for current VHAL interface, ID: " << propId;
        }

        return config.value();
    }

    getConfigsByPropId([this, &result, propId](const auto& configsByPropId) {
        SharedScopedLockAssertion lockAssertion(mConfigLock);

        auto it = configsByPropId.find(propId);
        if (it == configsByPropId.end()) {
            result = Error() << "no config for property, ID: " << propId;
            return;
        }
        // Copy the VehiclePropConfig
        result = it->second;
    });
    return result;
}

Result<void> DefaultVehicleHal::checkProperty(const VehiclePropValue& propValue) {
    int32_t propId = propValue.prop;
    auto result = getConfig(propId);
    if (!result.ok()) {
        return result.error();
    }
    const VehiclePropConfig& config = result.value();
    const VehicleAreaConfig* areaConfig = getAreaConfig(propValue, config);
    if (!isGlobalProp(propId) && areaConfig == nullptr) {
        // Ignore areaId for global property. For non global property, check whether areaId is
        // allowed. areaId must appear in areaConfig.
        return Error() << "invalid area ID: " << propValue.areaId << " for prop ID: " << propId
                       << ", not listed in config";
    }
    if (auto result = checkPropValue(propValue, &config); !result.ok()) {
        return Error() << "invalid property value: " << propValue.toString()
                       << ", error: " << getErrorMsg(result);
    }
    if (auto result = checkValueRange(propValue, areaConfig); !result.ok()) {
        return Error() << "property value out of range: " << propValue.toString()
                       << ", error: " << getErrorMsg(result);
    }
    return {};
}

ScopedAStatus DefaultVehicleHal::getValues(const CallbackType& callback,
                                           const GetValueRequests& requests) {
    ATRACE_CALL();
    if (callback == nullptr) {
        return ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }
    expected<LargeParcelableBase::BorrowedOwnedObject<GetValueRequests>, ScopedAStatus>
            deserializedResults = fromStableLargeParcelable(requests);
    if (!deserializedResults.ok()) {
        ALOGE("getValues: failed to parse getValues requests");
        return std::move(deserializedResults.error());
    }
    const std::vector<GetValueRequest>& getValueRequests =
            deserializedResults.value().getObject()->payloads;

    auto maybeRequestIds = checkDuplicateRequests(getValueRequests);
    if (!maybeRequestIds.ok()) {
        ALOGE("getValues: duplicate request ID");
        return toScopedAStatus(maybeRequestIds, StatusCode::INVALID_ARG);
    }

    // A list of failed result we already know before sending to hardware.
    std::vector<GetValueResult> failedResults;
    // The list of requests that we would send to hardware.
    std::vector<GetValueRequest> hardwareRequests;

    for (const auto& request : getValueRequests) {
        if (auto result = checkReadPermission(request.prop); !result.ok()) {
            ALOGW("property does not support reading: %s", getErrorMsg(result).c_str());
            failedResults.push_back(GetValueResult{
                    .requestId = request.requestId,
                    .status = getErrorCode(result),
                    .prop = {},
            });
            continue;
        }
        hardwareRequests.push_back(request);
    }

    // The set of request Ids that we would send to hardware.
    std::unordered_set<int64_t> hardwareRequestIds;
    for (const auto& request : hardwareRequests) {
        hardwareRequestIds.insert(request.requestId);
    }

    std::shared_ptr<GetValuesClient> client;
    {
        // Lock to make sure onBinderDied would not be called concurrently.
        std::scoped_lock lockGuard(mLock);
        if (!monitorBinderLifeCycleLocked(callback->asBinder().get())) {
            return ScopedAStatus::fromExceptionCodeWithMessage(EX_TRANSACTION_FAILED,
                                                               "client died");
        }

        client = getOrCreateClient(&mGetValuesClients, callback, mPendingRequestPool);
    }

    // Register the pending hardware requests and also check for duplicate request Ids.
    if (auto addRequestResult = client->addRequests(hardwareRequestIds); !addRequestResult.ok()) {
        ALOGE("getValues[%s]: failed to add pending requests, error: %s",
              toString(hardwareRequestIds).c_str(), getErrorMsg(addRequestResult).c_str());
        return toScopedAStatus(addRequestResult);
    }

    if (!failedResults.empty()) {
        // First send the failed results we already know back to the client.
        client->sendResults(std::move(failedResults));
    }

    if (hardwareRequests.empty()) {
        return ScopedAStatus::ok();
    }

    if (StatusCode status =
                mVehicleHardware->getValues(client->getResultCallback(), hardwareRequests);
        status != StatusCode::OK) {
        // If the hardware returns error, finish all the pending requests for this request because
        // we never expect hardware to call callback for these requests.
        client->tryFinishRequests(hardwareRequestIds);
        ALOGE("getValues[%s]: failed to get value from VehicleHardware, status: %d",
              toString(hardwareRequestIds).c_str(), toInt(status));
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                toInt(status), "failed to get value from VehicleHardware");
    }
    return ScopedAStatus::ok();
}

ScopedAStatus DefaultVehicleHal::setValues(const CallbackType& callback,
                                           const SetValueRequests& requests) {
    ATRACE_CALL();
    if (callback == nullptr) {
        return ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }
    expected<LargeParcelableBase::BorrowedOwnedObject<SetValueRequests>, ScopedAStatus>
            deserializedResults = fromStableLargeParcelable(requests);
    if (!deserializedResults.ok()) {
        ALOGE("setValues: failed to parse setValues requests");
        return std::move(deserializedResults.error());
    }
    const std::vector<SetValueRequest>& setValueRequests =
            deserializedResults.value().getObject()->payloads;

    // A list of failed result we already know before sending to hardware.
    std::vector<SetValueResult> failedResults;
    // The list of requests that we would send to hardware.
    std::vector<SetValueRequest> hardwareRequests;

    auto maybeRequestIds = checkDuplicateRequests(setValueRequests);
    if (!maybeRequestIds.ok()) {
        ALOGE("setValues: duplicate request ID");
        return toScopedAStatus(maybeRequestIds, StatusCode::INVALID_ARG);
    }

    for (auto& request : setValueRequests) {
        int64_t requestId = request.requestId;
        if (auto result = checkWritePermission(request.value); !result.ok()) {
            ALOGW("property does not support writing: %s", getErrorMsg(result).c_str());
            failedResults.push_back(SetValueResult{
                    .requestId = requestId,
                    .status = getErrorCode(result),
            });
            continue;
        }
        if (auto result = checkProperty(request.value); !result.ok()) {
            ALOGW("setValues[%" PRId64 "]: property is not valid: %s", requestId,
                  getErrorMsg(result).c_str());
            failedResults.push_back(SetValueResult{
                    .requestId = requestId,
                    .status = StatusCode::INVALID_ARG,
            });
            continue;
        }

        hardwareRequests.push_back(request);
    }

    // The set of request Ids that we would send to hardware.
    std::unordered_set<int64_t> hardwareRequestIds;
    for (const auto& request : hardwareRequests) {
        hardwareRequestIds.insert(request.requestId);
    }

    std::shared_ptr<SetValuesClient> client;
    {
        // Lock to make sure onBinderDied would not be called concurrently.
        std::scoped_lock lockGuard(mLock);
        if (!monitorBinderLifeCycleLocked(callback->asBinder().get())) {
            return ScopedAStatus::fromExceptionCodeWithMessage(EX_TRANSACTION_FAILED,
                                                               "client died");
        }
        client = getOrCreateClient(&mSetValuesClients, callback, mPendingRequestPool);
    }

    // Register the pending hardware requests and also check for duplicate request Ids.
    if (auto addRequestResult = client->addRequests(hardwareRequestIds); !addRequestResult.ok()) {
        ALOGE("setValues[%s], failed to add pending requests, error: %s",
              toString(hardwareRequestIds).c_str(), getErrorMsg(addRequestResult).c_str());
        return toScopedAStatus(addRequestResult);
    }

    if (!failedResults.empty()) {
        // First send the failed results we already know back to the client.
        client->sendResults(std::move(failedResults));
    }

    if (hardwareRequests.empty()) {
        return ScopedAStatus::ok();
    }

    if (StatusCode status =
                mVehicleHardware->setValues(client->getResultCallback(), hardwareRequests);
        status != StatusCode::OK) {
        // If the hardware returns error, finish all the pending requests for this request because
        // we never expect hardware to call callback for these requests.
        client->tryFinishRequests(hardwareRequestIds);
        ALOGE("setValues[%s], failed to set value to VehicleHardware, status: %d",
              toString(hardwareRequestIds).c_str(), toInt(status));
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                toInt(status), "failed to set value to VehicleHardware");
    }

    return ScopedAStatus::ok();
}

#define CHECK_DUPLICATE_REQUESTS(PROP_NAME)                                                      \
    do {                                                                                         \
        std::vector<int64_t> requestIds;                                                         \
        std::set<::aidl::android::hardware::automotive::vehicle::VehiclePropValue> requestProps; \
        for (const auto& request : requests) {                                                   \
            const auto& prop = request.PROP_NAME;                                                \
            if (requestProps.count(prop) != 0) {                                                 \
                return ::android::base::Error()                                                  \
                       << "duplicate request for property: " << prop.toString();                 \
            }                                                                                    \
            requestProps.insert(prop);                                                           \
            requestIds.push_back(request.requestId);                                             \
        }                                                                                        \
        return requestIds;                                                                       \
    } while (0);

::android::base::Result<std::vector<int64_t>> DefaultVehicleHal::checkDuplicateRequests(
        const std::vector<GetValueRequest>& requests) {
    CHECK_DUPLICATE_REQUESTS(prop);
}

::android::base::Result<std::vector<int64_t>> DefaultVehicleHal::checkDuplicateRequests(
        const std::vector<SetValueRequest>& requests) {
    CHECK_DUPLICATE_REQUESTS(value);
}

#undef CHECK_DUPLICATE_REQUESTS

ScopedAStatus DefaultVehicleHal::getPropConfigs(const std::vector<int32_t>& props,
                                                VehiclePropConfigs* output) {
    std::vector<VehiclePropConfig> configs;

    if (!mConfigInit) {
        for (int32_t prop : props) {
            auto maybeConfig = mVehicleHardware->getPropertyConfig(prop);
            if (!maybeConfig.has_value() ||
                !isConfigSupportedForCurrentVhalVersion(maybeConfig.value())) {
                return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                        toInt(StatusCode::INVALID_ARG),
                        StringPrintf("no config for property, ID: %" PRId32, prop).c_str());
            }
            configs.push_back(maybeConfig.value());
        }

        return vectorToStableLargeParcelable(std::move(configs), output);
    }

    ScopedAStatus status = ScopedAStatus::ok();
    getConfigsByPropId([this, &configs, &status, &props](const auto& configsByPropId) {
        SharedScopedLockAssertion lockAssertion(mConfigLock);

        for (int32_t prop : props) {
            auto it = configsByPropId.find(prop);
            if (it != configsByPropId.end()) {
                configs.push_back(it->second);
            } else {
                status = ScopedAStatus::fromServiceSpecificErrorWithMessage(
                        toInt(StatusCode::INVALID_ARG),
                        StringPrintf("no config for property, ID: %" PRId32, prop).c_str());
                return;
            }
        }
    });

    if (!status.isOk()) {
        return status;
    }

    return vectorToStableLargeParcelable(std::move(configs), output);
}

bool hasRequiredAccess(VehiclePropertyAccess access, VehiclePropertyAccess requiredAccess) {
    return access == requiredAccess || access == VehiclePropertyAccess::READ_WRITE;
}

bool areaConfigsHaveRequiredAccess(const std::vector<VehicleAreaConfig>& areaConfigs,
                                   VehiclePropertyAccess requiredAccess) {
    if (areaConfigs.empty()) {
        return false;
    }
    for (VehicleAreaConfig areaConfig : areaConfigs) {
        if (!hasRequiredAccess(areaConfig.access, requiredAccess)) {
            return false;
        }
    }
    return true;
}

VhalResult<void> DefaultVehicleHal::checkSubscribeOptions(
        const std::vector<SubscribeOptions>& options,
        const std::unordered_map<int32_t, VehiclePropConfig>& configsByPropId) {
    for (const auto& option : options) {
        int32_t propId = option.propId;
        auto it = configsByPropId.find(propId);
        if (it == configsByPropId.end()) {
            return StatusError(StatusCode::INVALID_ARG)
                   << StringPrintf("no config for property, ID: %" PRId32, propId);
        }
        const VehiclePropConfig& config = it->second;
        std::vector<VehicleAreaConfig> areaConfigs;
        if (option.areaIds.empty()) {
            areaConfigs = config.areaConfigs;
        } else {
            std::unordered_map<int, VehicleAreaConfig> areaConfigByAreaId;
            for (const VehicleAreaConfig& areaConfig : config.areaConfigs) {
                areaConfigByAreaId.emplace(areaConfig.areaId, areaConfig);
            }
            for (int areaId : option.areaIds) {
                auto it = areaConfigByAreaId.find(areaId);
                if (it != areaConfigByAreaId.end()) {
                    areaConfigs.push_back(it->second);
                } else if (areaId != 0 || !areaConfigByAreaId.empty()) {
                    return StatusError(StatusCode::INVALID_ARG)
                           << StringPrintf("invalid area ID: %" PRId32 " for prop ID: %" PRId32
                                           ", not listed in config",
                                           areaId, propId);
                }
            }
        }

        if (config.changeMode != VehiclePropertyChangeMode::ON_CHANGE &&
            config.changeMode != VehiclePropertyChangeMode::CONTINUOUS) {
            return StatusError(StatusCode::INVALID_ARG)
                   << "only support subscribing to ON_CHANGE or CONTINUOUS property";
        }

        // Either VehiclePropConfig.access or VehicleAreaConfig.access will be specified
        if (!hasRequiredAccess(config.access, VehiclePropertyAccess::READ) &&
            !areaConfigsHaveRequiredAccess(areaConfigs, VehiclePropertyAccess::READ)) {
            return StatusError(StatusCode::ACCESS_DENIED)
                   << StringPrintf("Property %" PRId32 " has no read access", propId);
        }

        if (config.changeMode == VehiclePropertyChangeMode::CONTINUOUS) {
            float sampleRateHz = option.sampleRate;
            float minSampleRateHz = config.minSampleRate;
            float maxSampleRateHz = config.maxSampleRate;
            float defaultRateHz =
                    getDefaultSampleRateHz(sampleRateHz, minSampleRateHz, maxSampleRateHz);
            if (sampleRateHz != defaultRateHz) {
                ALOGW("sample rate: %f HZ out of range, must be within %f HZ and %f HZ , set to %f "
                      "HZ",
                      sampleRateHz, minSampleRateHz, maxSampleRateHz, defaultRateHz);
                sampleRateHz = defaultRateHz;
            }
            if (!SubscriptionManager::checkSampleRateHz(sampleRateHz)) {
                return StatusError(StatusCode::INVALID_ARG)
                       << "invalid sample rate: " << sampleRateHz << " HZ";
            }
            if (!SubscriptionManager::checkResolution(option.resolution)) {
                return StatusError(StatusCode::INVALID_ARG)
                       << "invalid resolution: " << option.resolution;
            }
        }
    }

    return {};
}

void DefaultVehicleHal::parseSubscribeOptions(
        const std::vector<SubscribeOptions>& options,
        const std::unordered_map<int32_t, VehiclePropConfig>& configsByPropId,
        std::vector<SubscribeOptions>& onChangeSubscriptions,
        std::vector<SubscribeOptions>& continuousSubscriptions) {
    for (const auto& option : options) {
        int32_t propId = option.propId;
        // We have already validate config exists.
        const VehiclePropConfig& config = configsByPropId.at(propId);

        SubscribeOptions optionCopy = option;
        // If areaIds is empty, subscribe to all areas.
        if (optionCopy.areaIds.empty() && !isGlobalProp(propId)) {
            for (const auto& areaConfig : config.areaConfigs) {
                optionCopy.areaIds.push_back(areaConfig.areaId);
            }
        }

        if (isGlobalProp(propId)) {
            optionCopy.areaIds = {0};
        }

        if (config.changeMode == VehiclePropertyChangeMode::CONTINUOUS) {
            optionCopy.sampleRate = getDefaultSampleRateHz(
                    optionCopy.sampleRate, config.minSampleRate, config.maxSampleRate);
            if (!optionCopy.enableVariableUpdateRate) {
                continuousSubscriptions.push_back(std::move(optionCopy));
            } else {
                // If clients enables to VUR, we need to check whether VUR is supported for the
                // specific [propId, areaId] and overwrite the option to disable if not supported.
                std::vector<int32_t> areasVurEnabled;
                std::vector<int32_t> areasVurDisabled;
                for (int32_t areaId : optionCopy.areaIds) {
                    const VehicleAreaConfig* areaConfig = getAreaConfig(propId, areaId, config);
                    if (areaConfig == nullptr) {
                        areasVurDisabled.push_back(areaId);
                        continue;
                    }
                    if (!areaConfig->supportVariableUpdateRate) {
                        areasVurDisabled.push_back(areaId);
                        continue;
                    }
                    areasVurEnabled.push_back(areaId);
                }
                if (!areasVurEnabled.empty()) {
                    SubscribeOptions optionVurEnabled = optionCopy;
                    optionVurEnabled.areaIds = areasVurEnabled;
                    optionVurEnabled.enableVariableUpdateRate = true;
                    continuousSubscriptions.push_back(std::move(optionVurEnabled));
                }

                if (!areasVurDisabled.empty()) {
                    // We use optionCopy for areas with VUR disabled.
                    optionCopy.areaIds = areasVurDisabled;
                    optionCopy.enableVariableUpdateRate = false;
                    continuousSubscriptions.push_back(std::move(optionCopy));
                }
            }
        } else {
            onChangeSubscriptions.push_back(std::move(optionCopy));
        }
    }
}

ScopedAStatus DefaultVehicleHal::subscribe(const CallbackType& callback,
                                           const std::vector<SubscribeOptions>& options,
                                           [[maybe_unused]] int32_t maxSharedMemoryFileCount) {
    // TODO(b/205189110): Use shared memory file count.
    if (callback == nullptr) {
        return ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }
    std::vector<SubscribeOptions> onChangeSubscriptions;
    std::vector<SubscribeOptions> continuousSubscriptions;
    ScopedAStatus returnStatus = ScopedAStatus::ok();
    getConfigsByPropId([this, &returnStatus, &options, &onChangeSubscriptions,
                        &continuousSubscriptions](const auto& configsByPropId) {
        SharedScopedLockAssertion lockAssertion(mConfigLock);

        if (auto result = checkSubscribeOptions(options, configsByPropId); !result.ok()) {
            ALOGE("subscribe: invalid subscribe options: %s", getErrorMsg(result).c_str());
            returnStatus = toScopedAStatus(result);
            return;
        }
        parseSubscribeOptions(options, configsByPropId, onChangeSubscriptions,
                              continuousSubscriptions);
    });

    if (!returnStatus.isOk()) {
        return returnStatus;
    }

    {
        // Lock to make sure onBinderDied would not be called concurrently
        // (before subscribe). Without this, we may create a new subscription for an already dead
        // client which will never be unsubscribed.
        std::scoped_lock lockGuard(mLock);
        if (!monitorBinderLifeCycleLocked(callback->asBinder().get())) {
            return ScopedAStatus::fromExceptionCodeWithMessage(EX_TRANSACTION_FAILED,
                                                               "client died");
        }

        if (!onChangeSubscriptions.empty()) {
            auto result = mSubscriptionManager->subscribe(callback, onChangeSubscriptions,
                                                          /*isContinuousProperty=*/false);
            if (!result.ok()) {
                return toScopedAStatus(result);
            }
        }
        if (!continuousSubscriptions.empty()) {
            auto result = mSubscriptionManager->subscribe(callback, continuousSubscriptions,
                                                          /*isContinuousProperty=*/true);
            if (!result.ok()) {
                return toScopedAStatus(result);
            }
        }
    }
    return ScopedAStatus::ok();
}

ScopedAStatus DefaultVehicleHal::unsubscribe(const CallbackType& callback,
                                             const std::vector<int32_t>& propIds) {
    if (callback == nullptr) {
        return ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }
    return toScopedAStatus(mSubscriptionManager->unsubscribe(callback->asBinder().get(), propIds));
}

ScopedAStatus DefaultVehicleHal::returnSharedMemory(const CallbackType&, int64_t) {
    // TODO(b/200737967): implement this.
    return ScopedAStatus::ok();
}

Result<VehicleAreaConfig> DefaultVehicleHal::getAreaConfigForPropIdAreaId(int32_t propId,
                                                                          int32_t areaId) const {
    auto result = getConfig(propId);
    if (!result.ok()) {
        return Error() << "Failed to get property config for propertyId: " << propIdToString(propId)
                       << ", error: " << result.error();
    }
    const VehiclePropConfig& config = result.value();
    const VehicleAreaConfig* areaConfig = getAreaConfig(propId, areaId, config);
    if (areaConfig == nullptr) {
        return Error() << "AreaId config not found for propertyId: " << propIdToString(propId)
                       << ", areaId: " << areaId;
    }
    return *areaConfig;
}

Result<HasSupportedValueInfo> DefaultVehicleHal::getHasSupportedValueInfo(int32_t propId,
                                                                          int32_t areaId) const {
    Result<VehicleAreaConfig> propIdAreaIdConfigResult =
            getAreaConfigForPropIdAreaId(propId, areaId);
    if (!isGlobalProp(propId) && !propIdAreaIdConfigResult.ok()) {
        // For global property, it is possible that no config exists.
        return Error() << propIdAreaIdConfigResult.error();
    }
    if (propIdAreaIdConfigResult.has_value()) {
        auto areaConfig = propIdAreaIdConfigResult.value();
        if (areaConfig.hasSupportedValueInfo.has_value()) {
            return areaConfig.hasSupportedValueInfo.value();
        }
    }
    return Error() << "property: " << propIdToString(propId) << ", areaId: " << areaId
                   << " does not support this operation because hasSupportedValueInfo is null";
}

ScopedAStatus DefaultVehicleHal::getSupportedValuesLists(
        const std::vector<VhalPropIdAreaId>& vhalPropIdAreaIds,
        SupportedValuesListResults* supportedValuesListResults) {
    std::vector<size_t> toHardwareRequestCounters;
    std::vector<PropIdAreaId> toHardwarePropIdAreaIds;
    std::vector<SupportedValuesListResult> results;
    results.resize(vhalPropIdAreaIds.size());
    for (size_t requestCounter = 0; requestCounter < vhalPropIdAreaIds.size(); requestCounter++) {
        const auto& vhalPropIdAreaId = vhalPropIdAreaIds.at(requestCounter);
        int32_t propId = vhalPropIdAreaId.propId;
        int32_t areaId = vhalPropIdAreaId.areaId;
        auto hasSupportedValueInfoResult = getHasSupportedValueInfo(propId, areaId);
        if (!hasSupportedValueInfoResult.ok()) {
            ALOGE("getSupportedValuesLists: %s",
                  hasSupportedValueInfoResult.error().message().c_str());
            results[requestCounter] = SupportedValuesListResult{
                    .status = StatusCode::INVALID_ARG, .supportedValuesList = std::nullopt};
            continue;
        }

        const auto& hasSupportedValueInfo = hasSupportedValueInfoResult.value();
        if (hasSupportedValueInfo.hasSupportedValuesList) {
            toHardwarePropIdAreaIds.push_back(PropIdAreaId{.propId = propId, .areaId = areaId});
            toHardwareRequestCounters.push_back(requestCounter);
        } else {
            results[requestCounter] = SupportedValuesListResult{
                    .status = StatusCode::OK, .supportedValuesList = std::nullopt};
            continue;
        }
    }
    if (toHardwarePropIdAreaIds.size() != 0) {
        std::vector<SupportedValuesListResult> resultsFromHardware =
                mVehicleHardware->getSupportedValuesLists(toHardwarePropIdAreaIds);
        // It is guaranteed that toHardwarePropIdAreaIds, toHardwareRequestCounters,
        // resultsFromHardware have the same size.
        if (resultsFromHardware.size() != toHardwareRequestCounters.size()) {
            return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                    toInt(StatusCode::INTERNAL_ERROR),
                    fmt::format(
                            "getSupportedValuesLists: Unexpected results size from IVehicleHardware"
                            ", got: {}, expect: {}",
                            resultsFromHardware.size(), toHardwareRequestCounters.size())
                            .c_str());
        }
        for (size_t i = 0; i < toHardwareRequestCounters.size(); i++) {
            results[toHardwareRequestCounters[i]] = resultsFromHardware[i];
        }
    }
    ScopedAStatus status =
            vectorToStableLargeParcelable(std::move(results), supportedValuesListResults);
    if (!status.isOk()) {
        int statusCode = status.getServiceSpecificError();
        ALOGE("getSupportedValuesLists: failed to marshal result into large parcelable, error: "
              "%s, code: %d",
              status.getMessage(), statusCode);
        return status;
    }
    return ScopedAStatus::ok();
}

ScopedAStatus DefaultVehicleHal::getMinMaxSupportedValue(
        const std::vector<VhalPropIdAreaId>& vhalPropIdAreaIds,
        MinMaxSupportedValueResults* minMaxSupportedValueResults) {
    std::vector<size_t> toHardwareRequestCounters;
    std::vector<PropIdAreaId> toHardwarePropIdAreaIds;
    std::vector<MinMaxSupportedValueResult> results;
    results.resize(vhalPropIdAreaIds.size());
    for (size_t requestCounter = 0; requestCounter < vhalPropIdAreaIds.size(); requestCounter++) {
        const auto& vhalPropIdAreaId = vhalPropIdAreaIds.at(requestCounter);
        int32_t propId = vhalPropIdAreaId.propId;
        int32_t areaId = vhalPropIdAreaId.areaId;
        auto hasSupportedValueInfoResult = getHasSupportedValueInfo(propId, areaId);
        if (!hasSupportedValueInfoResult.ok()) {
            ALOGE("getMinMaxSupportedValue: %s",
                  hasSupportedValueInfoResult.error().message().c_str());
            results[requestCounter] = MinMaxSupportedValueResult{.status = StatusCode::INVALID_ARG,
                                                                 .minSupportedValue = std::nullopt,
                                                                 .maxSupportedValue = std::nullopt};
            continue;
        }

        const auto& hasSupportedValueInfo = hasSupportedValueInfoResult.value();
        if (hasSupportedValueInfo.hasMinSupportedValue ||
            hasSupportedValueInfo.hasMaxSupportedValue) {
            toHardwarePropIdAreaIds.push_back(PropIdAreaId{.propId = propId, .areaId = areaId});
            toHardwareRequestCounters.push_back(requestCounter);
        } else {
            results[requestCounter] = MinMaxSupportedValueResult{.status = StatusCode::OK,
                                                                 .minSupportedValue = std::nullopt,
                                                                 .maxSupportedValue = std::nullopt};
            continue;
        }
    }
    if (toHardwarePropIdAreaIds.size() != 0) {
        std::vector<MinMaxSupportedValueResult> resultsFromHardware =
                mVehicleHardware->getMinMaxSupportedValues(toHardwarePropIdAreaIds);
        // It is guaranteed that toHardwarePropIdAreaIds, toHardwareRequestCounters,
        // resultsFromHardware have the same size.
        if (resultsFromHardware.size() != toHardwareRequestCounters.size()) {
            return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                    toInt(StatusCode::INTERNAL_ERROR),
                    fmt::format(
                            "getMinMaxSupportedValue: Unexpected results size from IVehicleHardware"
                            ", got: {}, expect: {}",
                            resultsFromHardware.size(), toHardwareRequestCounters.size())
                            .c_str());
        }
        for (size_t i = 0; i < toHardwareRequestCounters.size(); i++) {
            results[toHardwareRequestCounters[i]] = resultsFromHardware[i];
        }
    }
    ScopedAStatus status =
            vectorToStableLargeParcelable(std::move(results), minMaxSupportedValueResults);
    if (!status.isOk()) {
        int statusCode = status.getServiceSpecificError();
        ALOGE("getMinMaxSupportedValue: failed to marshal result into large parcelable, error: "
              "%s, code: %d",
              status.getMessage(), statusCode);
        return status;
    }
    return ScopedAStatus::ok();
}

ScopedAStatus DefaultVehicleHal::registerSupportedValueChangeCallback(
        const std::shared_ptr<IVehicleCallback>& callback,
        const std::vector<VhalPropIdAreaId>& vhalPropIdAreaIds) {
    std::vector<PropIdAreaId> propIdAreaIdsToSubscribe;
    for (size_t i = 0; i < vhalPropIdAreaIds.size(); i++) {
        const auto& vhalPropIdAreaId = vhalPropIdAreaIds.at(i);
        int32_t propId = vhalPropIdAreaId.propId;
        int32_t areaId = vhalPropIdAreaId.areaId;
        auto hasSupportedValueInfoResult = getHasSupportedValueInfo(propId, areaId);
        if (!hasSupportedValueInfoResult.ok()) {
            ALOGE("registerSupportedValueChangeCallback not supported: %s",
                  hasSupportedValueInfoResult.error().message().c_str());
            return toScopedAStatus(hasSupportedValueInfoResult, StatusCode::INVALID_ARG);
        }
        const auto& hasSupportedValueInfo = hasSupportedValueInfoResult.value();
        if (!hasSupportedValueInfo.hasMinSupportedValue &&
            !hasSupportedValueInfo.hasMaxSupportedValue &&
            !hasSupportedValueInfo.hasSupportedValuesList) {
            ALOGW("registerSupportedValueChangeCallback: do nothing for property: %s, "
                  "areaId: %" PRId32
                  ", no min/max supported values or supported values list"
                  " specified",
                  propIdToString(propId).c_str(), areaId);
            continue;
        }
        propIdAreaIdsToSubscribe.push_back(PropIdAreaId{.propId = propId, .areaId = areaId});
    }
    if (propIdAreaIdsToSubscribe.empty()) {
        return ScopedAStatus::ok();
    }
    {
        // Lock to make sure onBinderDied would not be called concurrently
        // (before subscribeSupportedValueChange). Without this, we may create a new subscription
        // for an already dead client which will never be unsubscribed.
        std::scoped_lock lockGuard(mLock);
        if (!monitorBinderLifeCycleLocked(callback->asBinder().get())) {
            return ScopedAStatus::fromExceptionCodeWithMessage(EX_TRANSACTION_FAILED,
                                                               "client died");
        }
        auto result = mSubscriptionManager->subscribeSupportedValueChange(callback,
                                                                          propIdAreaIdsToSubscribe);
        if (!result.ok()) {
            ALOGW("registerSupportedValueChangeCallback: failed to subscribe supported value change"
                  " for %s, error: %s",
                  fmt::format("{}", propIdAreaIdsToSubscribe).c_str(),
                  result.error().message().c_str());
            return toScopedAStatus(result);
        }
    }
    return ScopedAStatus::ok();
}

ScopedAStatus DefaultVehicleHal::unregisterSupportedValueChangeCallback(
        const std::shared_ptr<IVehicleCallback>& callback,
        const std::vector<VhalPropIdAreaId>& vhalPropIdAreaIds) {
    std::vector<PropIdAreaId> propIdAreaIds;
    for (const auto& vhalPropIdAreaId : vhalPropIdAreaIds) {
        propIdAreaIds.push_back(
                PropIdAreaId{.propId = vhalPropIdAreaId.propId, .areaId = vhalPropIdAreaId.areaId});
    }

    auto result = mSubscriptionManager->unsubscribeSupportedValueChange(callback->asBinder().get(),
                                                                        propIdAreaIds);
    if (!result.ok()) {
        ALOGW("unregisterSupportedValueChangeCallback: failed to unsubscribe supported value change"
              " for %s, error: %s",
              fmt::format("{}", propIdAreaIds).c_str(), result.error().message().c_str());
        return toScopedAStatus(result);
    }
    return ScopedAStatus::ok();
}

IVehicleHardware* DefaultVehicleHal::getHardware() {
    return mVehicleHardware.get();
}

VhalResult<void> DefaultVehicleHal::checkPermissionHelper(
        const VehiclePropValue& value, VehiclePropertyAccess accessToTest) const {
    static const std::unordered_set<VehiclePropertyAccess> validAccesses = {
            VehiclePropertyAccess::WRITE, VehiclePropertyAccess::READ,
            VehiclePropertyAccess::READ_WRITE};
    if (validAccesses.find(accessToTest) == validAccesses.end()) {
        return StatusError(StatusCode::INVALID_ARG)
               << "checkPermissionHelper parameter is an invalid access type";
    }

    int32_t propId = value.prop;
    auto result = getConfig(propId);
    if (!result.ok()) {
        return StatusError(StatusCode::INVALID_ARG) << getErrorMsg(result);
    }

    const VehiclePropConfig& config = result.value();
    const VehicleAreaConfig* areaConfig = getAreaConfig(value, config);

    if (areaConfig == nullptr && !isGlobalProp(propId)) {
        return StatusError(StatusCode::INVALID_ARG) << "no config for area ID: " << value.areaId;
    }
    if (!hasRequiredAccess(config.access, accessToTest) &&
        (areaConfig == nullptr || !hasRequiredAccess(areaConfig->access, accessToTest))) {
        return StatusError(StatusCode::ACCESS_DENIED)
               << StringPrintf("Property %" PRId32 " does not have the following access: %" PRId32,
                               propId, static_cast<int32_t>(accessToTest));
    }
    return {};
}

VhalResult<void> DefaultVehicleHal::checkWritePermission(const VehiclePropValue& value) const {
    return checkPermissionHelper(value, VehiclePropertyAccess::WRITE);
}

VhalResult<void> DefaultVehicleHal::checkReadPermission(const VehiclePropValue& value) const {
    return checkPermissionHelper(value, VehiclePropertyAccess::READ);
}

void DefaultVehicleHal::checkHealth(IVehicleHardware* vehicleHardware,
                                    std::weak_ptr<SubscriptionManager> subscriptionManager) {
    StatusCode status = vehicleHardware->checkHealth();
    if (status != StatusCode::OK) {
        ALOGE("VHAL check health returns non-okay status");
        return;
    }
    std::vector<VehiclePropValue> values = {{
            .areaId = 0,
            .prop = toInt(VehicleProperty::VHAL_HEARTBEAT),
            .status = VehiclePropertyStatus::AVAILABLE,
            .value.int64Values = {uptimeMillis()},
    }};
    onPropertyChangeEvent(subscriptionManager, std::move(values));
    return;
}

binder_status_t DefaultVehicleHal::BinderLifecycleHandler::linkToDeath(
        AIBinder* binder, AIBinder_DeathRecipient* recipient, void* cookie) {
    return AIBinder_linkToDeath(binder, recipient, cookie);
}

bool DefaultVehicleHal::BinderLifecycleHandler::isAlive(const AIBinder* binder) {
    return AIBinder_isAlive(binder);
}

void DefaultVehicleHal::setBinderLifecycleHandler(
        std::unique_ptr<BinderLifecycleInterface> handler) {
    mBinderLifecycleHandler = std::move(handler);
}

bool DefaultVehicleHal::checkDumpPermission() {
    uid_t uid = AIBinder_getCallingUid();
    return uid == AID_ROOT || uid == AID_SHELL || uid == AID_SYSTEM;
}

binder_status_t DefaultVehicleHal::dump(int fd, const char** args, uint32_t numArgs) {
    if (!checkDumpPermission()) {
        dprintf(fd, "Caller must be root, system or shell");
        return STATUS_PERMISSION_DENIED;
    }

    std::vector<std::string> options;
    for (uint32_t i = 0; i < numArgs; i++) {
        options.push_back(args[i]);
    }
    if (options.size() == 1 && options[0] == "-a") {
        // Ignore "-a" option. Bugreport will call with this option.
        options.clear();
    }
    DumpResult result = mVehicleHardware->dump(options);
    if (result.refreshPropertyConfigs) {
        getAllPropConfigsFromHardwareLocked();
    }
    dprintf(fd, "%s", (result.buffer + "\n").c_str());
    if (!result.callerShouldDumpState) {
        return STATUS_OK;
    }
    dprintf(fd, "Vehicle HAL State: \n");
    std::unordered_map<int32_t, VehiclePropConfig> configsByPropIdCopy;
    getConfigsByPropId([this, &configsByPropIdCopy](const auto& configsByPropId) {
        SharedScopedLockAssertion lockAssertion(mConfigLock);

        configsByPropIdCopy = configsByPropId;
    });
    {
        std::scoped_lock<std::mutex> lockGuard(mLock);
        dprintf(fd, "Interface version: %" PRId32 "\n", getVhalInterfaceVersion());
        dprintf(fd, "Containing %zu property configs\n", configsByPropIdCopy.size());
        dprintf(fd, "Currently have %zu getValues clients\n", mGetValuesClients.size());
        dprintf(fd, "Currently have %zu setValues clients\n", mSetValuesClients.size());
        dprintf(fd, "Currently have %zu subscribe clients\n",
                mSubscriptionManager->countPropertyChangeClients());
        dprintf(fd, "Currently have %zu supported values change subscribe clients\n",
                mSubscriptionManager->countSupportedValueChangeClients());
    }
    return STATUS_OK;
}

size_t DefaultVehicleHal::countClients() {
    std::scoped_lock<std::mutex> lockGuard(mLock);
    return mGetValuesClients.size() + mSetValuesClients.size() +
           mSubscriptionManager->countPropertyChangeClients() +
           mSubscriptionManager->countSupportedValueChangeClients();
}

}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android
