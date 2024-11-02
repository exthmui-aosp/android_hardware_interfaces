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

#include "contexthub-impl/ContextHub.h"

namespace aidl::android::hardware::contexthub {

using ::ndk::ScopedAStatus;

ScopedAStatus ContextHub::getContextHubs(std::vector<ContextHubInfo>* out_contextHubInfos) {
    ContextHubInfo hub = {};
    hub.name = "Mock Context Hub";
    hub.vendor = "AOSP";
    hub.toolchain = "n/a";
    hub.id = kMockHubId;
    hub.peakMips = 1;
    hub.maxSupportedMessageLengthBytes = 4096;
    hub.chrePlatformId = UINT64_C(0x476f6f6754000000);
    hub.chreApiMajorVersion = 1;
    hub.chreApiMinorVersion = 6;
    hub.supportsReliableMessages = false;

    out_contextHubInfos->push_back(hub);

    return ScopedAStatus::ok();
}

// We don't expose any nanoapps for the default impl, therefore all nanoapp-related APIs fail.
ScopedAStatus ContextHub::loadNanoapp(int32_t /* in_contextHubId */,
                                      const NanoappBinary& /* in_appBinary */,
                                      int32_t /* in_transactionId */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus ContextHub::unloadNanoapp(int32_t /* in_contextHubId */, int64_t /* in_appId */,
                                        int32_t /* in_transactionId */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus ContextHub::disableNanoapp(int32_t /* in_contextHubId */, int64_t /* in_appId */,
                                         int32_t /* in_transactionId */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus ContextHub::enableNanoapp(int32_t /* in_contextHubId */, int64_t /* in_appId */,
                                        int32_t /* in_transactionId */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus ContextHub::onSettingChanged(Setting /* in_setting */, bool /*in_enabled */) {
    return ScopedAStatus::ok();
}

ScopedAStatus ContextHub::queryNanoapps(int32_t in_contextHubId) {
    if (in_contextHubId == kMockHubId && mCallback != nullptr) {
        std::vector<NanoappInfo> nanoapps;
        mCallback->handleNanoappInfo(nanoapps);
        return ScopedAStatus::ok();
    } else {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
}

ScopedAStatus ContextHub::getPreloadedNanoappIds(int32_t /* in_contextHubId */,
                                                 std::vector<int64_t>* out_preloadedNanoappIds) {
    if (out_preloadedNanoappIds == nullptr) {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    for (uint64_t i = 0; i < 10; ++i) {
        out_preloadedNanoappIds->push_back(i);
    }
    return ScopedAStatus::ok();
}

ScopedAStatus ContextHub::onNanSessionStateChanged(const NanSessionStateUpdate& /*in_update*/) {
    return ScopedAStatus::ok();
}

ScopedAStatus ContextHub::registerCallback(int32_t in_contextHubId,
                                           const std::shared_ptr<IContextHubCallback>& in_cb) {
    if (in_contextHubId == kMockHubId) {
        mCallback = in_cb;
        return ScopedAStatus::ok();
    } else {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
}

ScopedAStatus ContextHub::sendMessageToHub(int32_t in_contextHubId,
                                           const ContextHubMessage& /* in_message */) {
    if (in_contextHubId == kMockHubId) {
        // Return true here to indicate that the HAL has accepted the message.
        // Successful delivery of the message to a nanoapp should be handled at
        // a higher level protocol.
        return ScopedAStatus::ok();
    } else {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
}

ScopedAStatus ContextHub::setTestMode(bool /* enable */) {
    return ScopedAStatus::ok();
}

ScopedAStatus ContextHub::onHostEndpointConnected(const HostEndpointInfo& in_info) {
    mConnectedHostEndpoints.insert(in_info.hostEndpointId);

    return ScopedAStatus::ok();
}

ScopedAStatus ContextHub::onHostEndpointDisconnected(char16_t in_hostEndpointId) {
    if (mConnectedHostEndpoints.count(in_hostEndpointId) > 0) {
        mConnectedHostEndpoints.erase(in_hostEndpointId);
    }

    return ScopedAStatus::ok();
}

ScopedAStatus ContextHub::sendMessageDeliveryStatusToHub(
        int32_t /* in_contextHubId */,
        const MessageDeliveryStatus& /* in_messageDeliveryStatus */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus ContextHub::getHubs(std::vector<HubInfo>* _aidl_return) {
    ContextHubInfo hub = {};
    hub.name = "Mock Context Hub";
    hub.vendor = "AOSP";
    hub.toolchain = "n/a";
    hub.id = kMockHubId;
    hub.peakMips = 1;
    hub.maxSupportedMessageLengthBytes = 4096;
    hub.chrePlatformId = UINT64_C(0x476f6f6754000000);
    hub.chreApiMajorVersion = 1;
    hub.chreApiMinorVersion = 6;
    hub.supportsReliableMessages = false;

    HubInfo hubInfo1 = {};
    hubInfo1.hubId = hub.chrePlatformId;
    hubInfo1.hubDetails = HubInfo::HubDetails::make<HubInfo::HubDetails::Tag::contextHubInfo>(hub);

    VendorHubInfo vendorHub = {};
    vendorHub.name = "Mock Vendor Hub";
    vendorHub.version = 42;

    HubInfo hubInfo2 = {};
    hubInfo1.hubId = UINT64_C(0x1234567812345678);
    hubInfo1.hubDetails =
            HubInfo::HubDetails::make<HubInfo::HubDetails::Tag::vendorHubInfo>(vendorHub);

    _aidl_return->push_back(hubInfo1);
    _aidl_return->push_back(hubInfo2);

    return ScopedAStatus::ok();
};

ScopedAStatus ContextHub::getEndpoints(std::vector<EndpointInfo>* /* _aidl_return */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::registerEndpoint(const EndpointInfo& /* in_endpoint */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::unregisterEndpoint(const EndpointInfo& /* in_endpoint */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::registerEndpointCallback(
        const std::shared_ptr<IEndpointCallback>& /* in_callback */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::requestSessionIdRange(int32_t /* in_size */,
                                                std::vector<int32_t>* /* _aidl_return */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::openEndpointSession(
        int32_t /* in_sessionId */, const EndpointId& /* in_destination */,
        const EndpointId& /* in_initiator */,
        const std::optional<std::string>& /* in_serviceDescriptor */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::sendMessageToEndpoint(int32_t /* in_sessionId */,
                                                const Message& /* in_msg */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::sendMessageDeliveryStatusToEndpoint(
        int32_t /* in_sessionId */, const MessageDeliveryStatus& /* in_msgStatus */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::closeEndpointSession(int32_t /* in_sessionId */, Reason /* in_reason */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

ScopedAStatus ContextHub::endpointSessionOpenComplete(int32_t /* in_sessionId */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
};

}  // namespace aidl::android::hardware::contexthub
