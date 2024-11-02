/*
 * Copyright 2024 The Android Open Source Project
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

#pragma once

#include <aidl/android/hardware/security/keymint/IRemotelyProvisionedComponent.h>
#include <aidl/android/hardware/security/keymint/RpcHardwareInfo.h>
#include <android-base/properties.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>

namespace aidl::android::hardware::security::keymint::remote_prov {

using ::ndk::ScopedAStatus;

class MockIRemotelyProvisionedComponent : public IRemotelyProvisionedComponentDefault {
  public:
    MOCK_METHOD(ScopedAStatus, getHardwareInfo, (RpcHardwareInfo * _aidl_return), (override));
    MOCK_METHOD(ScopedAStatus, generateEcdsaP256KeyPair,
                (bool in_testMode, MacedPublicKey* out_macedPublicKey,
                 std::vector<uint8_t>* _aidl_return),
                (override));
    MOCK_METHOD(ScopedAStatus, generateCertificateRequest,
                (bool in_testMode, const std::vector<MacedPublicKey>& in_keysToSign,
                 const std::vector<uint8_t>& in_endpointEncryptionCertChain,
                 const std::vector<uint8_t>& in_challenge, DeviceInfo* out_deviceInfo,
                 ProtectedData* out_protectedData, std::vector<uint8_t>* _aidl_return),
                (override));
    MOCK_METHOD(ScopedAStatus, generateCertificateRequestV2,
                (const std::vector<MacedPublicKey>& in_keysToSign,
                 const std::vector<uint8_t>& in_challenge, std::vector<uint8_t>* _aidl_return),
                (override));
    MOCK_METHOD(ScopedAStatus, getInterfaceVersion, (int32_t* _aidl_return), (override));
    MOCK_METHOD(ScopedAStatus, getInterfaceHash, (std::string * _aidl_return), (override));
};

}  // namespace aidl::android::hardware::security::keymint::remote_prov