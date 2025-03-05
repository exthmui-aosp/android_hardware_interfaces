/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <aidl/android/hardware/security/see/hwcrypto/BnCryptoOperationContext.h>
#include <aidl/android/hardware/security/see/hwcrypto/BnHwCryptoOperations.h>
#include <aidl/android/hardware/security/see/hwcrypto/BnOpaqueKey.h>
#include <aidl/android/hardware/security/see/hwcrypto/IOpaqueKey.h>
#include <android-base/logging.h>
#include <android/hardware/security/see/hwcrypto/BnHwCryptoKey.h>
#include <binder/RpcTrusty.h>
#include <trusty/tipc.h>
#include <optional>
#include <string>
#include "hwcryptokeyimpl.h"

using android::IBinder;
using android::IInterface;
using android::RpcSession;
using android::RpcTrustyConnectWithSessionInitializer;
using android::sp;
using android::wp;
using android::base::ErrnoError;
using android::base::Error;
using android::base::Result;
using android::binder::Status;

namespace android {
namespace trusty {
namespace hwcryptohalservice {

#define HWCRYPTO_KEY_PORT "com.android.trusty.rust.hwcryptohal.V1"

// Even though we get the cpp_hwcrypto::IOpaqueKey and cpp_hwcrypto::ICryptoOperationContext and
// create the ndk_hwcrypto wrappers on this library we cannot cast them back when we need them
// because they are received on the function calls as binder objects and there is no reliable
// we to do this cast yet. Because of that we are creating maps to hold the wrapped objects
// and translate them on function calls.
// TODO: Add cleanup of both keyMapping and contextMapping once we have more test infrastructure in
//       place.
std::map<std::weak_ptr<ndk_hwcrypto::IOpaqueKey>, wp<cpp_hwcrypto::IOpaqueKey>, std::owner_less<>>
        keyMapping;
std::map<std::weak_ptr<ndk_hwcrypto::ICryptoOperationContext>,
         wp<cpp_hwcrypto::ICryptoOperationContext>, std::owner_less<>>
        contextMapping;

static ndk::ScopedAStatus convertStatus(Status status) {
    if (status.isOk()) {
        return ndk::ScopedAStatus::ok();
    } else {
        auto exCode = status.exceptionCode();
        if (exCode == Status::Exception::EX_SERVICE_SPECIFIC) {
            return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(
                    status.serviceSpecificErrorCode(), status.exceptionMessage());
        } else {
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(exCode,
                                                                    status.exceptionMessage());
        }
    }
}

static std::optional<cpp_hwcrypto::types::ExplicitKeyMaterial> convertExplicitKeyMaterial(
        const ndk_hwcrypto::types::ExplicitKeyMaterial& keyMaterial) {
    auto explicitKeyCpp = cpp_hwcrypto::types::ExplicitKeyMaterial();

    if (keyMaterial.getTag() == ndk_hwcrypto::types::ExplicitKeyMaterial::aes) {
        auto aesKey = keyMaterial.get<ndk_hwcrypto::types::ExplicitKeyMaterial::aes>();
        auto aesKeyCpp = cpp_hwcrypto::types::AesKey();
        if (aesKey.getTag() == ndk_hwcrypto::types::AesKey::aes128) {
            aesKeyCpp.set<cpp_hwcrypto::types::AesKey::aes128>(
                    aesKey.get<ndk_hwcrypto::types::AesKey::aes128>());
            explicitKeyCpp.set<cpp_hwcrypto::types::ExplicitKeyMaterial::aes>(aesKeyCpp);
        } else if (aesKey.getTag() == ndk_hwcrypto::types::AesKey::aes256) {
            aesKeyCpp.set<cpp_hwcrypto::types::AesKey::aes256>(
                    aesKey.get<ndk_hwcrypto::types::AesKey::aes256>());
            explicitKeyCpp.set<cpp_hwcrypto::types::ExplicitKeyMaterial::aes>(aesKeyCpp);
        } else {
            LOG(ERROR) << "unknown AesKey type";
            return std::nullopt;
        }
    } else if (keyMaterial.getTag() == ndk_hwcrypto::types::ExplicitKeyMaterial::hmac) {
        auto hmacKey = keyMaterial.get<ndk_hwcrypto::types::ExplicitKeyMaterial::hmac>();
        auto hmacKeyCpp = cpp_hwcrypto::types::HmacKey();
        if (hmacKey.getTag() == ndk_hwcrypto::types::HmacKey::sha256) {
            hmacKeyCpp.set<cpp_hwcrypto::types::HmacKey::sha256>(
                    hmacKey.get<ndk_hwcrypto::types::HmacKey::sha256>());
            explicitKeyCpp.set<cpp_hwcrypto::types::ExplicitKeyMaterial::hmac>(hmacKeyCpp);
        } else if (hmacKey.getTag() == ndk_hwcrypto::types::HmacKey::sha512) {
            hmacKeyCpp.set<cpp_hwcrypto::types::HmacKey::sha512>(
                    hmacKey.get<ndk_hwcrypto::types::HmacKey::sha512>());
            explicitKeyCpp.set<cpp_hwcrypto::types::ExplicitKeyMaterial::hmac>(hmacKeyCpp);
        } else {
            LOG(ERROR) << "unknown HmacKey type";
            return std::nullopt;
        }
    } else {
        LOG(ERROR) << "unknown Key type";
        return std::nullopt;
    }
    return explicitKeyCpp;
}

class HwCryptoOperationContextNdk : public ndk_hwcrypto::BnCryptoOperationContext {
  private:
    sp<cpp_hwcrypto::ICryptoOperationContext> mContext;

  public:
    HwCryptoOperationContextNdk(sp<cpp_hwcrypto::ICryptoOperationContext> operations)
        : mContext(std::move(operations)) {}

    static std::shared_ptr<HwCryptoOperationContextNdk> Create(
            sp<cpp_hwcrypto::ICryptoOperationContext> operations) {
        if (operations == nullptr) {
            return nullptr;
        }
        std::shared_ptr<HwCryptoOperationContextNdk> contextNdk =
                ndk::SharedRefBase::make<HwCryptoOperationContextNdk>(std::move(operations));

        if (!contextNdk) {
            LOG(ERROR) << "failed to allocate HwCryptoOperationContext";
            return nullptr;
        }
        return contextNdk;
    }
};

// TODO: Check refactoring opportunities like returning a Result<cpp_hwcrypto::types::OperationData>
//       once we add the code that uses this function.
Result<void> setOperationData(const ndk_hwcrypto::types::OperationData& ndkOperationData,
                              cpp_hwcrypto::types::OperationData* cppOperationData) {
    cpp_hwcrypto::types::MemoryBufferReference cppMemBuffRef;
    switch (ndkOperationData.getTag()) {
        case ndk_hwcrypto::types::OperationData::dataBuffer:
            cppOperationData->set<cpp_hwcrypto::types::OperationData::dataBuffer>(
                    ndkOperationData.get<ndk_hwcrypto::types::OperationData::dataBuffer>());
            break;
        case ndk_hwcrypto::types::OperationData::memoryBufferReference:
            cppMemBuffRef.startOffset =
                    ndkOperationData
                            .get<ndk_hwcrypto::types::OperationData::memoryBufferReference>()
                            .startOffset;
            cppMemBuffRef.sizeBytes =
                    ndkOperationData
                            .get<ndk_hwcrypto::types::OperationData::memoryBufferReference>()
                            .sizeBytes;
            cppOperationData->set<cpp_hwcrypto::types::OperationData::memoryBufferReference>(
                    std::move(cppMemBuffRef));
            break;
        default:
            // This shouldn't happen with the current definitions
            return ErrnoError() << "received unknown operation data type";
    }
    return {};
}

class HwCryptoOperationsNdk : public ndk_hwcrypto::BnHwCryptoOperations {
  private:
    sp<cpp_hwcrypto::IHwCryptoOperations> mHwCryptoOperations;

  public:
    HwCryptoOperationsNdk(sp<cpp_hwcrypto::IHwCryptoOperations> operations)
        : mHwCryptoOperations(std::move(operations)) {}

    static std::shared_ptr<HwCryptoOperationsNdk> Create(
            sp<cpp_hwcrypto::IHwCryptoOperations> operations) {
        if (operations == nullptr) {
            return nullptr;
        }
        std::shared_ptr<HwCryptoOperationsNdk> operationsNdk =
                ndk::SharedRefBase::make<HwCryptoOperationsNdk>(std::move(operations));

        if (!operationsNdk) {
            LOG(ERROR) << "failed to allocate HwCryptoOperations";
            return nullptr;
        }
        return operationsNdk;
    }

    ndk::ScopedAStatus processCommandList(
            std::vector<ndk_hwcrypto::CryptoOperationSet>* /*operationSets*/,
            std::vector<ndk_hwcrypto::CryptoOperationResult>* /*aidl_return*/) {
        return ndk::ScopedAStatus::ok();
    }
};

class OpaqueKeyNdk : public ndk_hwcrypto::BnOpaqueKey {
  private:
    sp<cpp_hwcrypto::IOpaqueKey> mOpaqueKey;

  public:
    OpaqueKeyNdk(sp<cpp_hwcrypto::IOpaqueKey> opaqueKey) : mOpaqueKey(std::move(opaqueKey)) {}

    static std::shared_ptr<OpaqueKeyNdk> Create(sp<cpp_hwcrypto::IOpaqueKey> opaqueKey) {
        if (opaqueKey == nullptr) {
            return nullptr;
        }
        std::shared_ptr<OpaqueKeyNdk> opaqueKeyNdk =
                ndk::SharedRefBase::make<OpaqueKeyNdk>(std::move(opaqueKey));

        if (!opaqueKeyNdk) {
            LOG(ERROR) << "failed to allocate HwCryptoKey";
            return nullptr;
        }
        return opaqueKeyNdk;
    }

    ndk::ScopedAStatus exportWrappedKey(
            const std::shared_ptr<ndk_hwcrypto::IOpaqueKey>& wrappingKey,
            ::std::vector<uint8_t>* aidl_return) {
        Status status = Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
        auto wrappingKeyNdk =
                retrieveCppBinder<cpp_hwcrypto::IOpaqueKey, ndk_hwcrypto::IOpaqueKey, keyMapping>(
                        wrappingKey);
        if (wrappingKeyNdk == nullptr) {
            LOG(ERROR) << "couldn't get wrapped key";
            return convertStatus(status);
        }
        status = mOpaqueKey->exportWrappedKey(wrappingKeyNdk, aidl_return);
        return convertStatus(status);
    }

    ndk::ScopedAStatus getKeyPolicy(ndk_hwcrypto::KeyPolicy* aidl_return) {
        Status status = Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
        if (aidl_return == nullptr) {
            LOG(ERROR) << "return value passed to getKeyPolicy is nullptr";
            return convertStatus(status);
        }
        cpp_hwcrypto::KeyPolicy cppPolicy = cpp_hwcrypto::KeyPolicy();

        status = mOpaqueKey->getKeyPolicy(&cppPolicy);
        if (status.isOk()) {
            auto ndkPolicy =
                    convertKeyPolicy<ndk_hwcrypto::KeyPolicy, cpp_hwcrypto::KeyPolicy>(cppPolicy);
            *aidl_return = std::move(ndkPolicy);
        }
        return convertStatus(status);
    }

    ndk::ScopedAStatus getPublicKey(::std::vector<uint8_t>* aidl_return) {
        auto status = mOpaqueKey->getPublicKey(aidl_return);
        return convertStatus(status);
    }

    ndk::ScopedAStatus getShareableToken(const ::std::vector<uint8_t>& sealingDicePolicy,
                                         ndk_hwcrypto::types::OpaqueKeyToken* aidl_return) {
        Status status = Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
        if (aidl_return == nullptr) {
            LOG(ERROR) << "return value passed to getShareableToken is nullptr";
            return convertStatus(status);
        }
        cpp_hwcrypto::types::OpaqueKeyToken binder_return;
        status = mOpaqueKey->getShareableToken(sealingDicePolicy, &binder_return);
        if (status.isOk()) {
            aidl_return->keyToken = std::move(binder_return.keyToken);
        }
        return convertStatus(status);
    }

    ndk::ScopedAStatus setProtectionId(
            const ndk_hwcrypto::types::ProtectionId /*protectionId*/,
            const ::std::vector<ndk_hwcrypto::types::OperationType>& /*allowedOperations*/) {
        return ndk::ScopedAStatus::ok();
    }
};

Result<void> HwCryptoKey::connectToTrusty(const char* tipcDev) {
    assert(!mSession);
    mSession = RpcTrustyConnectWithSessionInitializer(tipcDev, HWCRYPTO_KEY_PORT, [](auto) {});
    if (!mSession) {
        return ErrnoError() << "failed to connect to hwcrypto";
    }
    mRoot = mSession->getRootObject();
    mHwCryptoServer = cpp_hwcrypto::IHwCryptoKey::asInterface(mRoot);
    return {};
}

HwCryptoKey::HwCryptoKey() {}

std::shared_ptr<HwCryptoKey> HwCryptoKey::Create(const char* tipcDev) {
    std::shared_ptr<HwCryptoKey> hwCrypto = ndk::SharedRefBase::make<HwCryptoKey>();

    if (!hwCrypto) {
        LOG(ERROR) << "failed to allocate HwCryptoKey";
        return nullptr;
    }

    auto ret = hwCrypto->connectToTrusty(tipcDev);
    if (!ret.ok()) {
        LOG(ERROR) << "failed to connect HwCryptoKey to Trusty: " << ret.error();
        return nullptr;
    }

    return hwCrypto;
}

ndk::ScopedAStatus HwCryptoKey::deriveCurrentDicePolicyBoundKey(
        const ndk_hwcrypto::IHwCryptoKey::DiceBoundDerivationKey& /*derivationKey*/,
        ndk_hwcrypto::IHwCryptoKey::DiceCurrentBoundKeyResult* /*aidl_return*/) {
    // return mHwCryptoServer->deriveCurrentDicePolicyBoundKey(derivationKey, aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HwCryptoKey::deriveDicePolicyBoundKey(
        const ndk_hwcrypto::IHwCryptoKey::DiceBoundDerivationKey& /*derivationKey*/,
        const ::std::vector<uint8_t>& /*dicePolicyForKeyVersion*/,
        ndk_hwcrypto::IHwCryptoKey::DiceBoundKeyResult* /*aidl_return*/) {
    // return mHwCryptoServer->deriveDicePolicyBoundKey(derivationKey, dicePolicyForKeyVersion,
    // aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HwCryptoKey::deriveKey(
        const ndk_hwcrypto::IHwCryptoKey::DerivedKeyParameters& /*parameters*/,
        ndk_hwcrypto::IHwCryptoKey::DerivedKey* /*aidl_return*/) {
    // return mHwCryptoServer->deriveKey(parameters, aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus HwCryptoKey::getHwCryptoOperations(
        std::shared_ptr<ndk_hwcrypto::IHwCryptoOperations>* aidl_return) {
    Status status = Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
    if (aidl_return == nullptr) {
        LOG(ERROR) << "return value passed to getHwCryptoOperations is nullptr";
        return convertStatus(status);
    }
    sp<cpp_hwcrypto::IHwCryptoOperations> binder_return;
    status = mHwCryptoServer->getHwCryptoOperations(&binder_return);
    if (status.isOk()) {
        std::shared_ptr<ndk_hwcrypto::IHwCryptoOperations> operations =
                HwCryptoOperationsNdk::Create(binder_return);
        *aidl_return = operations;
    }
    return convertStatus(status);
}

ndk::ScopedAStatus HwCryptoKey::importClearKey(
        const ndk_hwcrypto::types::ExplicitKeyMaterial& keyMaterial,
        const ndk_hwcrypto::KeyPolicy& newKeyPolicy,
        std::shared_ptr<ndk_hwcrypto::IOpaqueKey>* aidl_return) {
    sp<cpp_hwcrypto::IOpaqueKey> binder_return = nullptr;
    Status status = Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
    if (aidl_return == nullptr) {
        LOG(ERROR) << "return value passed to importClearKey is nullptr";
        return convertStatus(status);
    }
    auto cppKeyPolicy =
            convertKeyPolicy<cpp_hwcrypto::KeyPolicy, ndk_hwcrypto::KeyPolicy>(newKeyPolicy);
    auto explicitKeyCpp = convertExplicitKeyMaterial(keyMaterial);
    if (!explicitKeyCpp.has_value()) {
        LOG(ERROR) << "couldn't convert key material";
        return convertStatus(status);
    }
    status = mHwCryptoServer->importClearKey(explicitKeyCpp.value(), cppKeyPolicy, &binder_return);
    if (status.isOk()) {
        if ((binder_return != nullptr)) {
            insertBinderMapping<cpp_hwcrypto::IOpaqueKey, ndk_hwcrypto::IOpaqueKey, OpaqueKeyNdk,
                                keyMapping>(binder_return, aidl_return);
        } else {
            *aidl_return = nullptr;
        }
    }
    return convertStatus(status);
}

ndk::ScopedAStatus HwCryptoKey::getCurrentDicePolicy(std::vector<uint8_t>* aidl_return) {
    auto status = mHwCryptoServer->getCurrentDicePolicy(aidl_return);
    return convertStatus(status);
}

ndk::ScopedAStatus HwCryptoKey::keyTokenImport(
        const ndk_hwcrypto::types::OpaqueKeyToken& requestedKey,
        const ::std::vector<uint8_t>& sealingDicePolicy,
        std::shared_ptr<ndk_hwcrypto::IOpaqueKey>* aidl_return) {
    Status status = Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
    if (aidl_return == nullptr) {
        LOG(ERROR) << "return value passed to keyTokenImport is nullptr";
        return convertStatus(status);
    }
    sp<cpp_hwcrypto::IOpaqueKey> binder_return;
    cpp_hwcrypto::types::OpaqueKeyToken requestedKeyCpp;
    // trying first a shallow copy of the vector
    requestedKeyCpp.keyToken = requestedKey.keyToken;
    status = mHwCryptoServer->keyTokenImport(requestedKeyCpp, sealingDicePolicy, &binder_return);
    if (status.isOk()) {
        std::shared_ptr<ndk_hwcrypto::IOpaqueKey> opaqueKey = OpaqueKeyNdk::Create(binder_return);
        *aidl_return = opaqueKey;
    }
    return convertStatus(status);
}

ndk::ScopedAStatus HwCryptoKey::getKeyslotData(
        ndk_hwcrypto::IHwCryptoKey::KeySlot /*slotId*/,
        std::shared_ptr<ndk_hwcrypto::IOpaqueKey>* /*aidl_return*/) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
            ndk_hwcrypto::types::HalErrorCode::UNAUTHORIZED);
}

}  // namespace hwcryptohalservice
}  // namespace trusty
}  // namespace android
