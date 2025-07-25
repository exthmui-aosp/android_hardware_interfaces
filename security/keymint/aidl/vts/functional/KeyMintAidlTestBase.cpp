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

#include "KeyMintAidlTestBase.h"

#include <chrono>
#include <fstream>
#include <unordered_set>
#include <vector>
#include "aidl/android/hardware/security/keymint/AttestationKey.h"
#include "aidl/android/hardware/security/keymint/ErrorCode.h"
#include "keymint_support/authorization_set.h"
#include "keymint_support/keymint_tags.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <android/content/pm/IPackageManagerNative.h>
#include <android_security_keystore2.h>
#include <cppbor_parse.h>
#include <cutils/properties.h>
#include <gmock/gmock.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <remote_prov/remote_prov_utils.h>
#include <vendorsupport/api_level.h>

#include <keymaster/cppcose/cppcose.h>
#include <keymint_support/key_param_output.h>
#include <keymint_support/keymint_utils.h>
#include <keymint_support/openssl_utils.h>

namespace aidl::android::hardware::security::keymint {

using namespace cppcose;
using namespace std::literals::chrono_literals;
using std::endl;
using std::optional;
using std::unique_ptr;
using ::testing::AssertionFailure;
using ::testing::AssertionResult;
using ::testing::AssertionSuccess;
using ::testing::ElementsAreArray;
using ::testing::MatchesRegex;
using ::testing::Not;

::std::ostream& operator<<(::std::ostream& os, const AuthorizationSet& set) {
    if (set.size() == 0)
        os << "(Empty)" << ::std::endl;
    else {
        os << "\n";
        for (auto& entry : set) os << entry << ::std::endl;
    }
    return os;
}

namespace test {

namespace {

// Possible values for the feature version.  Assumes that future KeyMint versions
// will continue with the 100 * AIDL_version numbering scheme.
//
// Must be kept in numerically increasing order.
const int32_t kFeatureVersions[] = {10,  11,  20,  30,  40,  41,  100, 200,
                                    300, 400, 500, 600, 700, 800, 900};

// Invalid value for a patchlevel (which is of form YYYYMMDD).
const uint32_t kInvalidPatchlevel = 99998877;

// Overhead for PKCS#1 v1.5 signature padding of undigested messages.  Digested messages have
// additional overhead, for the digest algorithmIdentifier required by PKCS#1.
const size_t kPkcs1UndigestedSignaturePaddingOverhead = 11;

// Determine whether the key description is for an asymmetric key.
bool is_asymmetric(const AuthorizationSet& key_desc) {
    auto algorithm = key_desc.GetTagValue(TAG_ALGORITHM);
    if (algorithm && (algorithm.value() == Algorithm::RSA || algorithm.value() == Algorithm::EC)) {
        return true;
    } else {
        return false;
    }
}

size_t count_tag_invalid_entries(const std::vector<KeyParameter>& authorizations) {
    return std::count_if(authorizations.begin(), authorizations.end(),
                         [](const KeyParameter& e) -> bool { return e.tag == Tag::INVALID; });
}

typedef KeyMintAidlTestBase::KeyData KeyData;
// Predicate for testing basic characteristics validity in generation or import.
bool KeyCharacteristicsBasicallyValid(SecurityLevel secLevel,
                                      const vector<KeyCharacteristics>& key_characteristics,
                                      int32_t aidl_version) {
    if (key_characteristics.empty()) return false;

    std::unordered_set<SecurityLevel> levels_seen;
    for (auto& entry : key_characteristics) {
        if (entry.authorizations.empty()) {
            GTEST_LOG_(ERROR) << "empty authorizations for " << entry.securityLevel;
            return false;
        }

        // There was no test to assert that INVALID tag should not present in authorization list
        // before Keymint V3, so there are some Keymint implementations where asserting for INVALID
        // tag fails(b/297306437), hence skipping for Keymint < 3.
        if (aidl_version >= 3) {
            EXPECT_EQ(count_tag_invalid_entries(entry.authorizations), 0);
        }

        // Just ignore the SecurityLevel::KEYSTORE as the KM won't do any enforcement on this.
        if (entry.securityLevel == SecurityLevel::KEYSTORE) continue;

        if (levels_seen.find(entry.securityLevel) != levels_seen.end()) {
            GTEST_LOG_(ERROR) << "duplicate authorizations for " << entry.securityLevel;
            return false;
        }
        levels_seen.insert(entry.securityLevel);

        // Generally, we should only have one entry, at the same security level as the KM
        // instance.  There is an exception: StrongBox KM can have some authorizations that are
        // enforced by the TEE.
        bool isExpectedSecurityLevel = secLevel == entry.securityLevel ||
                                       (secLevel == SecurityLevel::STRONGBOX &&
                                        entry.securityLevel == SecurityLevel::TRUSTED_ENVIRONMENT);

        if (!isExpectedSecurityLevel) {
            GTEST_LOG_(ERROR) << "Unexpected security level " << entry.securityLevel;
            return false;
        }
    }
    return true;
}

void check_crl_distribution_points_extension_not_present(X509* certificate) {
    ASN1_OBJECT_Ptr crl_dp_oid(OBJ_txt2obj(kCrlDPOid, 1 /* dotted string format */));
    ASSERT_TRUE(crl_dp_oid.get());

    int location =
            X509_get_ext_by_OBJ(certificate, crl_dp_oid.get(), -1 /* search from beginning */);
    ASSERT_EQ(location, -1);
}

void check_attestation_version(uint32_t attestation_version, int32_t aidl_version) {
    // Version numbers in attestation extensions should be a multiple of 100.
    EXPECT_EQ(attestation_version % 100, 0);

    // The multiplier should never be higher than the AIDL version, but can be less
    // (for example, if the implementation is from an earlier version but the HAL service
    // uses the default libraries and so reports the current AIDL version).
    EXPECT_LE((attestation_version / 100), aidl_version);
}

bool avb_verification_enabled() {
    char value[PROPERTY_VALUE_MAX];
    return property_get("ro.boot.vbmeta.device_state", value, "") != 0;
}

char nibble2hex[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                       '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

// Attestations don't completely align with key authorization lists, so we need to filter the lists
// to include only the tags that are in both.
auto kTagsToFilter = {
        Tag::CREATION_DATETIME,
        Tag::HARDWARE_TYPE,
        Tag::INCLUDE_UNIQUE_ID,
        Tag::MODULE_HASH,
};

AuthorizationSet filtered_tags(const AuthorizationSet& set) {
    AuthorizationSet filtered;
    std::remove_copy_if(
            set.begin(), set.end(), std::back_inserter(filtered), [](const auto& entry) -> bool {
                return std::find(kTagsToFilter.begin(), kTagsToFilter.end(), entry.tag) !=
                       kTagsToFilter.end();
            });
    return filtered;
}

// Remove any SecurityLevel::KEYSTORE entries from a list of key characteristics.
void strip_keystore_tags(vector<KeyCharacteristics>* characteristics) {
    characteristics->erase(std::remove_if(characteristics->begin(), characteristics->end(),
                                          [](const auto& entry) {
                                              return entry.securityLevel == SecurityLevel::KEYSTORE;
                                          }),
                           characteristics->end());
}

string x509NameToStr(X509_NAME* name) {
    char* s = X509_NAME_oneline(name, nullptr, 0);
    string retval(s);
    OPENSSL_free(s);
    return retval;
}

}  // namespace

bool KeyMintAidlTestBase::arm_deleteAllKeys = false;
bool KeyMintAidlTestBase::dump_Attestations = false;
std::string KeyMintAidlTestBase::keyblob_dir;
std::optional<bool> KeyMintAidlTestBase::expect_upgrade = std::nullopt;

KeyBlobDeleter::~KeyBlobDeleter() {
    if (key_blob_.empty()) {
        return;
    }
    Status result = keymint_->deleteKey(key_blob_);
    key_blob_.clear();
    EXPECT_TRUE(result.isOk()) << result.getServiceSpecificError() << "\n";
    ErrorCode rc = GetReturnErrorCode(result);
    EXPECT_TRUE(rc == ErrorCode::OK || rc == ErrorCode::UNIMPLEMENTED) << result << "\n";
}

uint32_t KeyMintAidlTestBase::boot_patch_level(
        const vector<KeyCharacteristics>& key_characteristics) {
    // The boot patchlevel is not available as a property, but should be present
    // in the key characteristics of any created key.
    AuthorizationSet allAuths;
    for (auto& entry : key_characteristics) {
        allAuths.push_back(AuthorizationSet(entry.authorizations));
    }
    auto patchlevel = allAuths.GetTagValue(TAG_BOOT_PATCHLEVEL);
    if (patchlevel.has_value()) {
        return patchlevel.value();
    } else {
        // No boot patchlevel is available. Return a value that won't match anything
        // and so will trigger test failures.
        return kInvalidPatchlevel;
    }
}

uint32_t KeyMintAidlTestBase::boot_patch_level() {
    return boot_patch_level(key_characteristics_);
}

std::optional<vector<uint8_t>> KeyMintAidlTestBase::getModuleHash() {
    if (AidlVersion() < 4) {
        // The `MODULE_HASH` tag was introduced in v4 of the HAL; earlier versions should never
        // expect to encounter it.
        return std::nullopt;
    }

    // The KeyMint instance should already have been informed of the `MODULE_HASH` value for the
    // currently running system. Generate a single attestation so we can find out what the value
    // is.
    auto challenge = "hello";
    auto app_id = "foo";
    auto params = AuthorizationSetBuilder()
                          .EcdsaSigningKey(EcCurve::P_256)
                          .Digest(Digest::NONE)
                          .Authorization(TAG_NO_AUTH_REQUIRED)
                          .AttestationChallenge(challenge)
                          .AttestationApplicationId(app_id)
                          .SetDefaultValidity();
    vector<uint8_t> key_blob;
    vector<KeyCharacteristics> key_characteristics;
    vector<Certificate> chain;
    auto result = GenerateKey(params, &key_blob, &key_characteristics, &chain);
    if (result != ErrorCode::OK) {
        ADD_FAILURE() << "Failed to generate attestation:" << result;
        return std::nullopt;
    }
    KeyBlobDeleter deleter(keymint_, key_blob);
    if (chain.empty()) {
        ADD_FAILURE() << "No attestation cert";
        return std::nullopt;
    }

    // Parse the attestation record in the leaf cert.
    X509_Ptr cert(parse_cert_blob(chain[0].encodedCertificate));
    if (cert.get() == nullptr) {
        ADD_FAILURE() << "Failed to parse attestation cert";
        return std::nullopt;
    }
    ASN1_OCTET_STRING* attest_rec = get_attestation_record(cert.get());
    if (attest_rec == nullptr) {
        ADD_FAILURE() << "Failed to find attestation extension";
        return std::nullopt;
    }
    AuthorizationSet att_sw_enforced;
    AuthorizationSet att_hw_enforced;
    uint32_t att_attestation_version;
    uint32_t att_keymint_version;
    SecurityLevel att_attestation_security_level;
    SecurityLevel att_keymint_security_level;
    vector<uint8_t> att_challenge;
    vector<uint8_t> att_unique_id;
    vector<uint8_t> att_app_id;

    auto error = parse_attestation_record(attest_rec->data,                 //
                                          attest_rec->length,               //
                                          &att_attestation_version,         //
                                          &att_attestation_security_level,  //
                                          &att_keymint_version,             //
                                          &att_keymint_security_level,      //
                                          &att_challenge,                   //
                                          &att_sw_enforced,                 //
                                          &att_hw_enforced,                 //
                                          &att_unique_id);
    if (error != ErrorCode::OK) {
        ADD_FAILURE() << "Failed to parse attestation extension";
        return std::nullopt;
    }

    // The module hash should be present in the software-enforced list.
    if (!att_sw_enforced.Contains(TAG_MODULE_HASH)) {
        ADD_FAILURE() << "No TAG_MODULE_HASH in attestation extension";
        return std::nullopt;
    }
    return att_sw_enforced.GetTagValue(TAG_MODULE_HASH);
}

/**
 * An API to determine device IDs attestation is required or not,
 * which is mandatory for KeyMint version 2 and first_api_level 33 or greater.
 */
bool KeyMintAidlTestBase::isDeviceIdAttestationRequired() {
    if (!is_gsi_image()) {
        return AidlVersion() >= 2 &&
            get_vendor_api_level() >= AVendorSupport_getVendorApiLevelOf(__ANDROID_API_T__);
    } else {
        // The device ID properties may not be set properly when testing earlier implementations
        // under GSI, e.g. `ro.product.<id>` is overridden by the GSI image, but the
        // `ro.product.vendor.<id>` value (which does survive GSI installation) was not set.
        return AidlVersion() >= 2 &&
            get_vendor_api_level() >= AVendorSupport_getVendorApiLevelOf(__ANDROID_API_U__);
    }
}

/**
 * An API to determine second IMEI ID attestation is required or not,
 * which is supported for KeyMint version 3 or first_api_level greater than 33.
 */
bool KeyMintAidlTestBase::isSecondImeiIdAttestationRequired() {
    return AidlVersion() >= 3 && property_get_int32("ro.vendor.api_level", 0) > __ANDROID_API_T__;
}

std::optional<bool> KeyMintAidlTestBase::isRkpOnly() {
    // GSI replaces the values for remote_prov_prop properties (since they’re system_internal_prop
    // properties), so on GSI the properties are not reliable indicators of whether StrongBox/TEE is
    // RKP-only or not.
    if (is_gsi_image()) {
        return std::nullopt;
    }
    if (SecLevel() == SecurityLevel::STRONGBOX) {
        return property_get_bool("remote_provisioning.strongbox.rkp_only", false);
    }
    return property_get_bool("remote_provisioning.tee.rkp_only", false);
}

bool KeyMintAidlTestBase::Curve25519Supported() {
    // Strongbox never supports curve 25519.
    if (SecLevel() == SecurityLevel::STRONGBOX) {
        return false;
    }

    // Curve 25519 was included in version 2 of the KeyMint interface.
    return AidlVersion() >= 2;
}

void KeyMintAidlTestBase::InitializeKeyMint(std::shared_ptr<IKeyMintDevice> keyMint) {
    ASSERT_NE(keyMint, nullptr);
    keymint_ = std::move(keyMint);

    KeyMintHardwareInfo info;
    ASSERT_TRUE(keymint_->getHardwareInfo(&info).isOk());

    securityLevel_ = info.securityLevel;
    name_.assign(info.keyMintName.begin(), info.keyMintName.end());
    author_.assign(info.keyMintAuthorName.begin(), info.keyMintAuthorName.end());
    timestamp_token_required_ = info.timestampTokenRequired;

    os_version_ = getOsVersion();
    os_patch_level_ = getOsPatchlevel();
    vendor_patch_level_ = getVendorPatchlevel();

    if (!::android::security::keystore2::attest_modules()) {
        // Some tests (for v4+) require that the KeyMint instance has been
        // provided with a module hash value.  If the keystore2 flag is off,
        // this will not happen, so set a fake value here instead.
        GTEST_LOG_(INFO) << "Setting MODULE_HASH to fake value as fallback when flag off";
        vector<uint8_t> fakeModuleHash = {
                0xf3, 0xf1, 0x1f, 0xe5, 0x13, 0x05, 0xfe, 0xfa, 0xe9, 0xc3, 0x53,
                0xef, 0x69, 0xdf, 0x9f, 0xd7, 0x0c, 0x1e, 0xcc, 0x2c, 0x2c, 0x62,
                0x1f, 0x5e, 0x2c, 0x1d, 0x19, 0xa1, 0xfd, 0xac, 0xa1, 0xb4,
        };
        vector<KeyParameter> info = {Authorization(TAG_MODULE_HASH, fakeModuleHash)};
        keymint_->setAdditionalAttestationInfo(info);
    }
}

int32_t KeyMintAidlTestBase::AidlVersion() const {
    int32_t version = 0;
    auto status = keymint_->getInterfaceVersion(&version);
    if (!status.isOk()) {
        ADD_FAILURE() << "Failed to determine interface version";
    }
    return version;
}

void KeyMintAidlTestBase::SetUp() {
    if (AServiceManager_isDeclared(GetParam().c_str())) {
        ::ndk::SpAIBinder binder(AServiceManager_waitForService(GetParam().c_str()));
        InitializeKeyMint(IKeyMintDevice::fromBinder(binder));
    } else {
        InitializeKeyMint(nullptr);
    }
}

ErrorCode KeyMintAidlTestBase::GenerateKey(const AuthorizationSet& key_desc) {
    return GenerateKey(key_desc, &key_blob_, &key_characteristics_);
}

ErrorCode KeyMintAidlTestBase::GenerateKey(const AuthorizationSet& key_desc,
                                           vector<uint8_t>* key_blob,
                                           vector<KeyCharacteristics>* key_characteristics) {
    return GenerateKey(key_desc, key_blob, key_characteristics, &cert_chain_);
}

ErrorCode KeyMintAidlTestBase::GenerateKey(const AuthorizationSet& key_desc,
                                           vector<uint8_t>* key_blob,
                                           vector<KeyCharacteristics>* key_characteristics,
                                           vector<Certificate>* cert_chain) {
    std::optional<AttestationKey> attest_key = std::nullopt;
    vector<Certificate> attest_cert_chain;
    // If an attestation is requested, but the system is RKP-only, we need to supply an explicit
    // attestation key. Else the result is a key without an attestation.
    // - If the RKP-only value is undeterminable (i.e., when running on GSI), generate and use the
    //   `ATTEST_KEY` anyways.
    // - In the case that using an `ATTEST_KEY` is not supported
    //   (shouldSkipAttestKeyTest), assume the device has factory keys (so not RKP-only).
    // - If the key being generated is a symmetric key (from test cases that check that the
    //   attestation parameters are correctly ignored), don't try to use an `ATTEST_KEY`.
    if (isRkpOnly().value_or(true) && key_desc.Contains(TAG_ATTESTATION_CHALLENGE) &&
        !shouldSkipAttestKeyTest() && is_asymmetric(key_desc)) {
        AuthorizationSet attest_key_desc =
                AuthorizationSetBuilder().EcdsaKey(EcCurve::P_256).AttestKey().SetDefaultValidity();
        attest_key.emplace();
        vector<KeyCharacteristics> attest_key_characteristics;
        auto error = GenerateAttestKey(attest_key_desc, std::nullopt, &attest_key.value().keyBlob,
                                       &attest_key_characteristics, &attest_cert_chain);
        EXPECT_EQ(error, ErrorCode::OK);
        EXPECT_EQ(attest_cert_chain.size(), 1);
        attest_key.value().issuerSubjectName = make_name_from_str("Android Keystore Key");
    }

    ErrorCode error = GenerateKey(key_desc, attest_key, key_blob, key_characteristics, cert_chain);

    if (error == ErrorCode::OK && attest_cert_chain.size() > 0) {
        cert_chain->push_back(attest_cert_chain[0]);
    }

    return error;
}

ErrorCode KeyMintAidlTestBase::GenerateKey(const AuthorizationSet& key_desc,
                                           const optional<AttestationKey>& attest_key,
                                           vector<uint8_t>* key_blob,
                                           vector<KeyCharacteristics>* key_characteristics,
                                           vector<Certificate>* cert_chain) {
    EXPECT_NE(key_blob, nullptr) << "Key blob pointer must not be null.  Test bug";
    EXPECT_NE(key_characteristics, nullptr)
            << "Previous characteristics not deleted before generating key.  Test bug.";

    KeyCreationResult creationResult;
    Status result = keymint_->generateKey(key_desc.vector_data(), attest_key, &creationResult);
    if (result.isOk()) {
        EXPECT_PRED3(KeyCharacteristicsBasicallyValid, SecLevel(),
                     creationResult.keyCharacteristics, AidlVersion());
        EXPECT_GT(creationResult.keyBlob.size(), 0);
        *key_blob = std::move(creationResult.keyBlob);
        *key_characteristics = std::move(creationResult.keyCharacteristics);
        *cert_chain = std::move(creationResult.certificateChain);

        if (is_asymmetric(key_desc)) {
            EXPECT_GE(cert_chain->size(), 1);
            if (key_desc.Contains(TAG_ATTESTATION_CHALLENGE)) {
                if (attest_key) {
                    EXPECT_EQ(cert_chain->size(), 1);
                } else {
                    EXPECT_GT(cert_chain->size(), 1);
                }
            }
        } else {
            // For symmetric keys there should be no certificates.
            EXPECT_EQ(cert_chain->size(), 0);
        }
    }

    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::ImportKey(const AuthorizationSet& key_desc, KeyFormat format,
                                         const string& key_material, vector<uint8_t>* key_blob,
                                         vector<KeyCharacteristics>* key_characteristics) {
    Status result;

    cert_chain_.clear();
    key_characteristics->clear();
    key_blob->clear();

    KeyCreationResult creationResult;
    result = keymint_->importKey(key_desc.vector_data(), format,
                                 vector<uint8_t>(key_material.begin(), key_material.end()),
                                 {} /* attestationSigningKeyBlob */, &creationResult);

    if (result.isOk()) {
        EXPECT_PRED3(KeyCharacteristicsBasicallyValid, SecLevel(),
                     creationResult.keyCharacteristics, AidlVersion());
        EXPECT_GT(creationResult.keyBlob.size(), 0);

        *key_blob = std::move(creationResult.keyBlob);
        *key_characteristics = std::move(creationResult.keyCharacteristics);
        cert_chain_ = std::move(creationResult.certificateChain);

        if (is_asymmetric(key_desc)) {
            EXPECT_GE(cert_chain_.size(), 1);
            if (key_desc.Contains(TAG_ATTESTATION_CHALLENGE)) EXPECT_GT(cert_chain_.size(), 1);
        } else {
            // For symmetric keys there should be no certificates.
            EXPECT_EQ(cert_chain_.size(), 0);
        }
    }

    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::ImportKey(const AuthorizationSet& key_desc, KeyFormat format,
                                         const string& key_material) {
    return ImportKey(key_desc, format, key_material, &key_blob_, &key_characteristics_);
}

ErrorCode KeyMintAidlTestBase::ImportWrappedKey(string wrapped_key, string wrapping_key,
                                                const AuthorizationSet& wrapping_key_desc,
                                                string masking_key,
                                                const AuthorizationSet& unwrapping_params,
                                                int64_t password_sid, int64_t biometric_sid) {
    EXPECT_EQ(ErrorCode::OK, ImportKey(wrapping_key_desc, KeyFormat::PKCS8, wrapping_key));

    key_characteristics_.clear();

    KeyCreationResult creationResult;
    Status result = keymint_->importWrappedKey(
            vector<uint8_t>(wrapped_key.begin(), wrapped_key.end()), key_blob_,
            vector<uint8_t>(masking_key.begin(), masking_key.end()),
            unwrapping_params.vector_data(), password_sid, biometric_sid, &creationResult);

    if (result.isOk()) {
        EXPECT_PRED3(KeyCharacteristicsBasicallyValid, SecLevel(),
                     creationResult.keyCharacteristics, AidlVersion());
        EXPECT_GT(creationResult.keyBlob.size(), 0);

        key_blob_ = std::move(creationResult.keyBlob);
        key_characteristics_ = std::move(creationResult.keyCharacteristics);
        cert_chain_ = std::move(creationResult.certificateChain);

        AuthorizationSet allAuths;
        for (auto& entry : key_characteristics_) {
            allAuths.push_back(AuthorizationSet(entry.authorizations));
        }
        if (is_asymmetric(allAuths)) {
            EXPECT_GE(cert_chain_.size(), 1);
        } else {
            // For symmetric keys there should be no certificates.
            EXPECT_EQ(cert_chain_.size(), 0);
        }
    }

    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::GetCharacteristics(const vector<uint8_t>& key_blob,
                                                  const vector<uint8_t>& app_id,
                                                  const vector<uint8_t>& app_data,
                                                  vector<KeyCharacteristics>* key_characteristics) {
    Status result =
            keymint_->getKeyCharacteristics(key_blob, app_id, app_data, key_characteristics);
    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::GetCharacteristics(const vector<uint8_t>& key_blob,
                                                  vector<KeyCharacteristics>* key_characteristics) {
    vector<uint8_t> empty_app_id, empty_app_data;
    return GetCharacteristics(key_blob, empty_app_id, empty_app_data, key_characteristics);
}

void KeyMintAidlTestBase::CheckCharacteristics(
        const vector<uint8_t>& key_blob,
        const vector<KeyCharacteristics>& generate_characteristics) {
    // Any key characteristics that were in SecurityLevel::KEYSTORE when returned from
    // generateKey() should be excluded, as KeyMint will have no record of them.
    // This applies to CREATION_DATETIME in particular.
    vector<KeyCharacteristics> expected_characteristics(generate_characteristics);
    strip_keystore_tags(&expected_characteristics);

    vector<KeyCharacteristics> retrieved;
    ASSERT_EQ(ErrorCode::OK, GetCharacteristics(key_blob, &retrieved));
    EXPECT_EQ(expected_characteristics, retrieved);
}

void KeyMintAidlTestBase::CheckAppIdCharacteristics(
        const vector<uint8_t>& key_blob, std::string_view app_id_string,
        std::string_view app_data_string,
        const vector<KeyCharacteristics>& generate_characteristics) {
    // Exclude any SecurityLevel::KEYSTORE characteristics for comparisons.
    vector<KeyCharacteristics> expected_characteristics(generate_characteristics);
    strip_keystore_tags(&expected_characteristics);

    vector<uint8_t> app_id(app_id_string.begin(), app_id_string.end());
    vector<uint8_t> app_data(app_data_string.begin(), app_data_string.end());
    vector<KeyCharacteristics> retrieved;
    ASSERT_EQ(ErrorCode::OK, GetCharacteristics(key_blob, app_id, app_data, &retrieved));
    EXPECT_EQ(expected_characteristics, retrieved);

    // Check that key characteristics can't be retrieved if the app ID or app data is missing.
    vector<uint8_t> empty;
    vector<KeyCharacteristics> not_retrieved;
    EXPECT_EQ(ErrorCode::INVALID_KEY_BLOB,
              GetCharacteristics(key_blob, empty, app_data, &not_retrieved));
    EXPECT_EQ(not_retrieved.size(), 0);

    EXPECT_EQ(ErrorCode::INVALID_KEY_BLOB,
              GetCharacteristics(key_blob, app_id, empty, &not_retrieved));
    EXPECT_EQ(not_retrieved.size(), 0);

    EXPECT_EQ(ErrorCode::INVALID_KEY_BLOB,
              GetCharacteristics(key_blob, empty, empty, &not_retrieved));
    EXPECT_EQ(not_retrieved.size(), 0);
}

ErrorCode KeyMintAidlTestBase::DeleteKey(vector<uint8_t>* key_blob, bool keep_key_blob) {
    Status result = keymint_->deleteKey(*key_blob);
    if (!keep_key_blob) {
        *key_blob = vector<uint8_t>();
    }

    EXPECT_TRUE(result.isOk()) << result.getServiceSpecificError() << endl;
    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::DeleteKey(bool keep_key_blob) {
    return DeleteKey(&key_blob_, keep_key_blob);
}

ErrorCode KeyMintAidlTestBase::DeleteAllKeys() {
    Status result = keymint_->deleteAllKeys();
    EXPECT_TRUE(result.isOk()) << result.getServiceSpecificError() << endl;
    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::DestroyAttestationIds() {
    Status result = keymint_->destroyAttestationIds();
    return GetReturnErrorCode(result);
}

void KeyMintAidlTestBase::CheckedDeleteKey() {
    ErrorCode result = DeleteKey(&key_blob_, /* keep_key_blob = */ false);
    EXPECT_TRUE(result == ErrorCode::OK || result == ErrorCode::UNIMPLEMENTED) << result << endl;
}

ErrorCode KeyMintAidlTestBase::Begin(KeyPurpose purpose, const vector<uint8_t>& key_blob,
                                     const AuthorizationSet& in_params,
                                     AuthorizationSet* out_params,
                                     std::shared_ptr<IKeyMintOperation>& op) {
    SCOPED_TRACE("Begin");
    Status result;
    BeginResult out;
    result = keymint_->begin(purpose, key_blob, in_params.vector_data(), std::nullopt, &out);

    if (result.isOk()) {
        *out_params = out.params;
        challenge_ = out.challenge;
        op = out.operation;
    }

    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::Begin(KeyPurpose purpose, const vector<uint8_t>& key_blob,
                                     const AuthorizationSet& in_params,
                                     AuthorizationSet* out_params,
                                     std::optional<HardwareAuthToken> hat) {
    SCOPED_TRACE("Begin");
    Status result;
    BeginResult out;

    result = keymint_->begin(purpose, key_blob, in_params.vector_data(), hat, &out);

    if (result.isOk()) {
        *out_params = out.params;
        challenge_ = out.challenge;
        op_ = out.operation;
    }

    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::Begin(KeyPurpose purpose, const AuthorizationSet& in_params,
                                     AuthorizationSet* out_params) {
    SCOPED_TRACE("Begin");
    EXPECT_EQ(nullptr, op_);
    return Begin(purpose, key_blob_, in_params, out_params);
}

ErrorCode KeyMintAidlTestBase::Begin(KeyPurpose purpose, const AuthorizationSet& in_params) {
    SCOPED_TRACE("Begin");
    AuthorizationSet out_params;
    ErrorCode result = Begin(purpose, in_params, &out_params);
    EXPECT_TRUE(out_params.empty());
    return result;
}

ErrorCode KeyMintAidlTestBase::UpdateAad(const string& input) {
    return GetReturnErrorCode(op_->updateAad(vector<uint8_t>(input.begin(), input.end()),
                                             {} /* hardwareAuthToken */,
                                             {} /* verificationToken */));
}

ErrorCode KeyMintAidlTestBase::Update(const string& input, string* output) {
    SCOPED_TRACE("Update");

    Status result;
    if (!output) return ErrorCode::UNEXPECTED_NULL_POINTER;

    EXPECT_NE(op_, nullptr);
    if (!op_) return ErrorCode::UNEXPECTED_NULL_POINTER;

    std::vector<uint8_t> o_put;
    result = op_->update(vector<uint8_t>(input.begin(), input.end()), {}, {}, &o_put);

    if (result.isOk()) {
        output->append(o_put.begin(), o_put.end());
    } else {
        // Failure always terminates the operation.
        op_ = {};
    }

    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::Finish(const string& input, const string& signature, string* output,
                                      std::optional<HardwareAuthToken> hat,
                                      std::optional<secureclock::TimeStampToken> time_token) {
    SCOPED_TRACE("Finish");
    Status result;

    EXPECT_NE(op_, nullptr);
    if (!op_) return ErrorCode::UNEXPECTED_NULL_POINTER;

    vector<uint8_t> oPut;
    result = op_->finish(vector<uint8_t>(input.begin(), input.end()),
                         vector<uint8_t>(signature.begin(), signature.end()), hat, time_token,
                         {} /* confirmationToken */, &oPut);

    if (result.isOk()) output->append(oPut.begin(), oPut.end());

    op_ = {};
    return GetReturnErrorCode(result);
}

ErrorCode KeyMintAidlTestBase::Abort(const std::shared_ptr<IKeyMintOperation>& op) {
    SCOPED_TRACE("Abort");

    EXPECT_NE(op, nullptr);
    if (!op) return ErrorCode::UNEXPECTED_NULL_POINTER;

    Status retval = op->abort();
    EXPECT_TRUE(retval.isOk());
    return static_cast<ErrorCode>(retval.getServiceSpecificError());
}

ErrorCode KeyMintAidlTestBase::Abort() {
    SCOPED_TRACE("Abort");

    EXPECT_NE(op_, nullptr);
    if (!op_) return ErrorCode::UNEXPECTED_NULL_POINTER;

    Status retval = op_->abort();
    return static_cast<ErrorCode>(retval.getServiceSpecificError());
}

void KeyMintAidlTestBase::AbortIfNeeded() {
    SCOPED_TRACE("AbortIfNeeded");
    if (op_) {
        EXPECT_EQ(ErrorCode::OK, Abort());
        op_.reset();
    }
}

auto KeyMintAidlTestBase::ProcessMessage(const vector<uint8_t>& key_blob, KeyPurpose operation,
                                         const string& message, const AuthorizationSet& in_params)
        -> std::tuple<ErrorCode, string> {
    AuthorizationSet begin_out_params;
    ErrorCode result = Begin(operation, key_blob, in_params, &begin_out_params);
    if (result != ErrorCode::OK) return {result, {}};

    string output;
    return {Finish(message, &output), output};
}

string KeyMintAidlTestBase::ProcessMessage(const vector<uint8_t>& key_blob, KeyPurpose operation,
                                           const string& message, const AuthorizationSet& in_params,
                                           AuthorizationSet* out_params) {
    SCOPED_TRACE("ProcessMessage");
    AuthorizationSet begin_out_params;
    ErrorCode result = Begin(operation, key_blob, in_params, out_params);
    EXPECT_EQ(ErrorCode::OK, result);
    if (result != ErrorCode::OK) {
        return "";
    }

    string output;
    EXPECT_EQ(ErrorCode::OK, Finish(message, &output));
    return output;
}

string KeyMintAidlTestBase::SignMessage(const vector<uint8_t>& key_blob, const string& message,
                                        const AuthorizationSet& params) {
    SCOPED_TRACE("SignMessage");
    AuthorizationSet out_params;
    string signature = ProcessMessage(key_blob, KeyPurpose::SIGN, message, params, &out_params);
    EXPECT_TRUE(out_params.empty());
    return signature;
}

string KeyMintAidlTestBase::SignMessage(const string& message, const AuthorizationSet& params) {
    SCOPED_TRACE("SignMessage");
    return SignMessage(key_blob_, message, params);
}

string KeyMintAidlTestBase::MacMessage(const string& message, Digest digest, size_t mac_length) {
    SCOPED_TRACE("MacMessage");
    return SignMessage(
            key_blob_, message,
            AuthorizationSetBuilder().Digest(digest).Authorization(TAG_MAC_LENGTH, mac_length));
}

void KeyMintAidlTestBase::CheckAesIncrementalEncryptOperation(BlockMode block_mode,
                                                              int message_size) {
    auto builder = AuthorizationSetBuilder()
                           .Authorization(TAG_NO_AUTH_REQUIRED)
                           .AesEncryptionKey(128)
                           .BlockMode(block_mode)
                           .Padding(PaddingMode::NONE);
    if (block_mode == BlockMode::GCM) {
        builder.Authorization(TAG_MIN_MAC_LENGTH, 128);
    }
    ASSERT_EQ(ErrorCode::OK, GenerateKey(builder));

    for (int increment = 1; increment <= message_size; ++increment) {
        string message(message_size, 'a');
        auto params = AuthorizationSetBuilder().BlockMode(block_mode).Padding(PaddingMode::NONE);
        if (block_mode == BlockMode::GCM) {
            params.Authorization(TAG_MAC_LENGTH, 128) /* for GCM */;
        }

        AuthorizationSet output_params;
        EXPECT_EQ(ErrorCode::OK, Begin(KeyPurpose::ENCRYPT, params, &output_params));

        string ciphertext;
        string to_send;
        for (size_t i = 0; i < message.size(); i += increment) {
            EXPECT_EQ(ErrorCode::OK, Update(message.substr(i, increment), &ciphertext));
        }
        EXPECT_EQ(ErrorCode::OK, Finish(to_send, &ciphertext))
                << "Error sending " << to_send << " with block mode " << block_mode;

        switch (block_mode) {
            case BlockMode::GCM:
                EXPECT_EQ(message.size() + 16, ciphertext.size());
                break;
            case BlockMode::CTR:
                EXPECT_EQ(message.size(), ciphertext.size());
                break;
            case BlockMode::CBC:
            case BlockMode::ECB:
                EXPECT_EQ(message.size() + message.size() % 16, ciphertext.size());
                break;
        }

        auto iv = output_params.GetTagValue(TAG_NONCE);
        switch (block_mode) {
            case BlockMode::CBC:
            case BlockMode::GCM:
            case BlockMode::CTR:
                ASSERT_TRUE(iv) << "No IV for block mode " << block_mode;
                EXPECT_EQ(block_mode == BlockMode::GCM ? 12U : 16U, iv->get().size());
                params.push_back(TAG_NONCE, iv->get());
                break;

            case BlockMode::ECB:
                EXPECT_FALSE(iv) << "ECB mode should not generate IV";
                break;
        }

        EXPECT_EQ(ErrorCode::OK, Begin(KeyPurpose::DECRYPT, params))
                << "Decrypt begin() failed for block mode " << block_mode;

        string plaintext;
        for (size_t i = 0; i < ciphertext.size(); i += increment) {
            EXPECT_EQ(ErrorCode::OK, Update(ciphertext.substr(i, increment), &plaintext));
        }
        ErrorCode error = Finish(to_send, &plaintext);
        ASSERT_EQ(ErrorCode::OK, error) << "Decryption failed for block mode " << block_mode
                                        << " and increment " << increment;
        if (error == ErrorCode::OK) {
            ASSERT_EQ(message, plaintext) << "Decryption didn't match for block mode " << block_mode
                                          << " and increment " << increment;
        }
    }
}

void KeyMintAidlTestBase::AesCheckEncryptOneByteAtATime(const string& key, BlockMode block_mode,
                                                        PaddingMode padding_mode, const string& iv,
                                                        const string& plaintext,
                                                        const string& exp_cipher_text) {
    bool is_authenticated_cipher = (block_mode == BlockMode::GCM);
    auto auth_set = AuthorizationSetBuilder()
                            .Authorization(TAG_NO_AUTH_REQUIRED)
                            .AesEncryptionKey(key.size() * 8)
                            .BlockMode(block_mode)
                            .Padding(padding_mode);
    if (iv.size() > 0) auth_set.Authorization(TAG_CALLER_NONCE);
    if (is_authenticated_cipher) auth_set.Authorization(TAG_MIN_MAC_LENGTH, 128);
    ASSERT_EQ(ErrorCode::OK, ImportKey(auth_set, KeyFormat::RAW, key));

    CheckEncryptOneByteAtATime(block_mode, 16 /*block_size*/, padding_mode, iv, plaintext,
                               exp_cipher_text);
}

void KeyMintAidlTestBase::CheckEncryptOneByteAtATime(BlockMode block_mode, const int block_size,
                                                     PaddingMode padding_mode, const string& iv,
                                                     const string& plaintext,
                                                     const string& exp_cipher_text) {
    bool is_stream_cipher = (block_mode == BlockMode::CTR || block_mode == BlockMode::GCM);
    bool is_authenticated_cipher = (block_mode == BlockMode::GCM);
    auto params = AuthorizationSetBuilder().BlockMode(block_mode).Padding(padding_mode);
    if (iv.size() > 0) params.Authorization(TAG_NONCE, iv.data(), iv.size());
    if (is_authenticated_cipher) params.Authorization(TAG_MAC_LENGTH, 128);

    AuthorizationSet output_params;
    EXPECT_EQ(ErrorCode::OK, Begin(KeyPurpose::ENCRYPT, params, &output_params));

    string actual_ciphertext;
    if (is_stream_cipher) {
        // Assert that a 1 byte of output is produced for 1 byte of input.
        // Every input byte produces an output byte.
        for (int plaintext_index = 0; plaintext_index < plaintext.size(); plaintext_index++) {
            string ciphertext;
            EXPECT_EQ(ErrorCode::OK, Update(plaintext.substr(plaintext_index, 1), &ciphertext));
            // Some StrongBox implementations cannot support 1:1 input:output lengths, so
            // we relax this API restriction for them.
            if (SecLevel() != SecurityLevel::STRONGBOX) {
                EXPECT_EQ(1, ciphertext.size()) << "plaintext index: " << plaintext_index;
            }
            actual_ciphertext.append(ciphertext);
        }
        string ciphertext;
        EXPECT_EQ(ErrorCode::OK, Finish(&ciphertext));
        if (SecLevel() != SecurityLevel::STRONGBOX) {
            string expected_final_output;
            if (is_authenticated_cipher) {
                expected_final_output = exp_cipher_text.substr(plaintext.size());
            }
            EXPECT_EQ(expected_final_output, ciphertext);
        }
        actual_ciphertext.append(ciphertext);
    } else {
        // Assert that a block of output is produced once a full block of input is provided.
        // Every input block produces an output block.
        bool compare_output = true;
        string additional_information;
        int vendor_api_level = property_get_int32("ro.vendor.api_level", 0);
        if (SecLevel() == SecurityLevel::STRONGBOX) {
            // This is known to be broken on older vendor implementations.
            if (vendor_api_level <= __ANDROID_API_U__) {
                compare_output = false;
            } else {
                additional_information = " (b/194134359) ";
            }
        }
        for (int plaintext_index = 0; plaintext_index < plaintext.size(); plaintext_index++) {
            string ciphertext;
            EXPECT_EQ(ErrorCode::OK, Update(plaintext.substr(plaintext_index, 1), &ciphertext));
            if (compare_output) {
                if ((plaintext_index % block_size) == block_size - 1) {
                    // Update is expected to have output a new block
                    EXPECT_EQ(block_size, ciphertext.size())
                            << "plaintext index: " << plaintext_index << additional_information;
                } else {
                    // Update is expected to have produced no output
                    EXPECT_EQ(0, ciphertext.size())
                            << "plaintext index: " << plaintext_index << additional_information;
                }
            }
            actual_ciphertext.append(ciphertext);
        }
        string ciphertext;
        EXPECT_EQ(ErrorCode::OK, Finish(&ciphertext));
        actual_ciphertext.append(ciphertext);
    }
    // Regardless of how the completed ciphertext got accumulated, it should match the expected
    // ciphertext.
    EXPECT_EQ(exp_cipher_text, actual_ciphertext);
}

void KeyMintAidlTestBase::CheckHmacTestVector(const string& key, const string& message,
                                              Digest digest, const string& expected_mac) {
    SCOPED_TRACE("CheckHmacTestVector");
    ASSERT_EQ(ErrorCode::OK,
              ImportKey(AuthorizationSetBuilder()
                                .Authorization(TAG_NO_AUTH_REQUIRED)
                                .HmacKey(key.size() * 8)
                                .Authorization(TAG_MIN_MAC_LENGTH, expected_mac.size() * 8)
                                .Digest(digest),
                        KeyFormat::RAW, key));
    string signature = MacMessage(message, digest, expected_mac.size() * 8);
    EXPECT_EQ(expected_mac, signature)
            << "Test vector didn't match for key of size " << key.size() << " message of size "
            << message.size() << " and digest " << digest;
    CheckedDeleteKey();
}

void KeyMintAidlTestBase::CheckAesCtrTestVector(const string& key, const string& nonce,
                                                const string& message,
                                                const string& expected_ciphertext) {
    SCOPED_TRACE("CheckAesCtrTestVector");
    ASSERT_EQ(ErrorCode::OK, ImportKey(AuthorizationSetBuilder()
                                               .Authorization(TAG_NO_AUTH_REQUIRED)
                                               .AesEncryptionKey(key.size() * 8)
                                               .BlockMode(BlockMode::CTR)
                                               .Authorization(TAG_CALLER_NONCE)
                                               .Padding(PaddingMode::NONE),
                                       KeyFormat::RAW, key));

    auto params = AuthorizationSetBuilder()
                          .Authorization(TAG_NONCE, nonce.data(), nonce.size())
                          .BlockMode(BlockMode::CTR)
                          .Padding(PaddingMode::NONE);
    AuthorizationSet out_params;
    string ciphertext = EncryptMessage(key_blob_, message, params, &out_params);
    EXPECT_EQ(expected_ciphertext, ciphertext);
}

void KeyMintAidlTestBase::CheckTripleDesTestVector(KeyPurpose purpose, BlockMode block_mode,
                                                   PaddingMode padding_mode, const string& key,
                                                   const string& iv, const string& input,
                                                   const string& expected_output) {
    auto authset = AuthorizationSetBuilder()
                           .TripleDesEncryptionKey(key.size() * 7)
                           .BlockMode(block_mode)
                           .Authorization(TAG_NO_AUTH_REQUIRED)
                           .Padding(padding_mode);
    if (iv.size()) authset.Authorization(TAG_CALLER_NONCE);
    ASSERT_EQ(ErrorCode::OK, ImportKey(authset, KeyFormat::RAW, key));
    ASSERT_GT(key_blob_.size(), 0U);

    auto begin_params = AuthorizationSetBuilder().BlockMode(block_mode).Padding(padding_mode);
    if (iv.size()) begin_params.Authorization(TAG_NONCE, iv.data(), iv.size());
    AuthorizationSet output_params;
    string output = ProcessMessage(key_blob_, purpose, input, begin_params, &output_params);
    EXPECT_EQ(expected_output, output);
}

void KeyMintAidlTestBase::VerifyMessage(const vector<uint8_t>& key_blob, const string& message,
                                        const string& signature, const AuthorizationSet& params) {
    SCOPED_TRACE("VerifyMessage");
    AuthorizationSet begin_out_params;
    ASSERT_EQ(ErrorCode::OK, Begin(KeyPurpose::VERIFY, key_blob, params, &begin_out_params));

    string output;
    EXPECT_EQ(ErrorCode::OK, Finish(message, signature, &output));
    EXPECT_TRUE(output.empty());
    op_ = {};
}

void KeyMintAidlTestBase::VerifyMessage(const string& message, const string& signature,
                                        const AuthorizationSet& params) {
    SCOPED_TRACE("VerifyMessage");
    VerifyMessage(key_blob_, message, signature, params);
}

void KeyMintAidlTestBase::LocalVerifyMessage(const string& message, const string& signature,
                                             const AuthorizationSet& params) {
    SCOPED_TRACE("LocalVerifyMessage");

    ASSERT_GT(cert_chain_.size(), 0);
    LocalVerifyMessage(cert_chain_[0].encodedCertificate, message, signature, params);
}

void KeyMintAidlTestBase::LocalVerifyMessage(const vector<uint8_t>& der_cert, const string& message,
                                             const string& signature,
                                             const AuthorizationSet& params) {
    // Retrieve the public key from the leaf certificate.
    X509_Ptr key_cert(parse_cert_blob(der_cert));
    ASSERT_TRUE(key_cert.get());
    EVP_PKEY_Ptr pub_key(X509_get_pubkey(key_cert.get()));
    ASSERT_TRUE(pub_key.get());

    Digest digest = params.GetTagValue(TAG_DIGEST).value();
    PaddingMode padding = PaddingMode::NONE;
    auto tag = params.GetTagValue(TAG_PADDING);
    if (tag.has_value()) {
        padding = tag.value();
    }

    if (digest == Digest::NONE) {
        switch (EVP_PKEY_id(pub_key.get())) {
            case EVP_PKEY_ED25519: {
                ASSERT_EQ(64, signature.size());
                uint8_t pub_keydata[32];
                size_t pub_len = sizeof(pub_keydata);
                ASSERT_EQ(1, EVP_PKEY_get_raw_public_key(pub_key.get(), pub_keydata, &pub_len));
                ASSERT_EQ(sizeof(pub_keydata), pub_len);
                ASSERT_EQ(1, ED25519_verify(reinterpret_cast<const uint8_t*>(message.data()),
                                            message.size(),
                                            reinterpret_cast<const uint8_t*>(signature.data()),
                                            pub_keydata));
                break;
            }

            case EVP_PKEY_EC: {
                vector<uint8_t> data((EVP_PKEY_bits(pub_key.get()) + 7) / 8);
                size_t data_size = std::min(data.size(), message.size());
                memcpy(data.data(), message.data(), data_size);
                EC_KEY_Ptr ecdsa(EVP_PKEY_get1_EC_KEY(pub_key.get()));
                ASSERT_TRUE(ecdsa.get());
                ASSERT_EQ(1,
                          ECDSA_verify(0, reinterpret_cast<const uint8_t*>(data.data()), data_size,
                                       reinterpret_cast<const uint8_t*>(signature.data()),
                                       signature.size(), ecdsa.get()));
                break;
            }
            case EVP_PKEY_RSA: {
                vector<uint8_t> data(EVP_PKEY_size(pub_key.get()));
                size_t data_size = std::min(data.size(), message.size());
                memcpy(data.data(), message.data(), data_size);

                RSA_Ptr rsa(EVP_PKEY_get1_RSA(const_cast<EVP_PKEY*>(pub_key.get())));
                ASSERT_TRUE(rsa.get());

                size_t key_len = RSA_size(rsa.get());
                int openssl_padding = RSA_NO_PADDING;
                switch (padding) {
                    case PaddingMode::NONE:
                        ASSERT_LE(data_size, key_len);
                        ASSERT_EQ(key_len, signature.size());
                        openssl_padding = RSA_NO_PADDING;
                        break;
                    case PaddingMode::RSA_PKCS1_1_5_SIGN:
                        ASSERT_LE(data_size + kPkcs1UndigestedSignaturePaddingOverhead, key_len);
                        openssl_padding = RSA_PKCS1_PADDING;
                        break;
                    default:
                        ADD_FAILURE() << "Unsupported RSA padding mode " << padding;
                }

                vector<uint8_t> decrypted_data(key_len);
                int bytes_decrypted = RSA_public_decrypt(
                        signature.size(), reinterpret_cast<const uint8_t*>(signature.data()),
                        decrypted_data.data(), rsa.get(), openssl_padding);
                ASSERT_GE(bytes_decrypted, 0);

                const uint8_t* compare_pos = decrypted_data.data();
                size_t bytes_to_compare = bytes_decrypted;
                uint8_t zero_check_result = 0;
                if (padding == PaddingMode::NONE && data_size < bytes_to_compare) {
                    // If the data is short, for "unpadded" signing we zero-pad to the left.  So
                    // during verification we should have zeros on the left of the decrypted data.
                    // Do a constant-time check.
                    const uint8_t* zero_end = compare_pos + bytes_to_compare - data_size;
                    while (compare_pos < zero_end) zero_check_result |= *compare_pos++;
                    ASSERT_EQ(0, zero_check_result);
                    bytes_to_compare = data_size;
                }
                ASSERT_EQ(0, memcmp(compare_pos, data.data(), bytes_to_compare));
                break;
            }
            default:
                ADD_FAILURE() << "Unknown public key type";
        }
    } else {
        EVP_MD_CTX digest_ctx;
        EVP_MD_CTX_init(&digest_ctx);
        EVP_PKEY_CTX* pkey_ctx;
        const EVP_MD* md = openssl_digest(digest);
        ASSERT_NE(md, nullptr);
        ASSERT_EQ(1, EVP_DigestVerifyInit(&digest_ctx, &pkey_ctx, md, nullptr, pub_key.get()));

        if (padding == PaddingMode::RSA_PSS) {
            EXPECT_GT(EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, RSA_PKCS1_PSS_PADDING), 0);
            EXPECT_GT(EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_ctx, EVP_MD_size(md)), 0);
            EXPECT_GT(EVP_PKEY_CTX_set_rsa_mgf1_md(pkey_ctx, md), 0);
        }

        ASSERT_EQ(1, EVP_DigestVerifyUpdate(&digest_ctx,
                                            reinterpret_cast<const uint8_t*>(message.data()),
                                            message.size()));
        ASSERT_EQ(1, EVP_DigestVerifyFinal(&digest_ctx,
                                           reinterpret_cast<const uint8_t*>(signature.data()),
                                           signature.size()));
        EVP_MD_CTX_cleanup(&digest_ctx);
    }
}

string KeyMintAidlTestBase::LocalRsaEncryptMessage(const string& message,
                                                   const AuthorizationSet& params) {
    SCOPED_TRACE("LocalRsaEncryptMessage");

    // Retrieve the public key from the leaf certificate.
    if (cert_chain_.empty()) {
        ADD_FAILURE() << "No public key available";
        return "Failure";
    }
    X509_Ptr key_cert(parse_cert_blob(cert_chain_[0].encodedCertificate));
    if (key_cert.get() == nullptr) {
        ADD_FAILURE() << "Failed to parse cert";
        return "Failure";
    }
    EVP_PKEY_Ptr pub_key(X509_get_pubkey(key_cert.get()));
    if (pub_key.get() == nullptr) {
        ADD_FAILURE() << "Failed to retrieve public key";
        return "Failure";
    }
    RSA_Ptr rsa(EVP_PKEY_get1_RSA(const_cast<EVP_PKEY*>(pub_key.get())));
    if (rsa.get() == nullptr) {
        ADD_FAILURE() << "Failed to retrieve RSA public key";
        return "Failure";
    }

    // Retrieve relevant tags.
    Digest digest = Digest::NONE;
    Digest mgf_digest = Digest::SHA1;
    PaddingMode padding = PaddingMode::NONE;

    auto digest_tag = params.GetTagValue(TAG_DIGEST);
    if (digest_tag.has_value()) digest = digest_tag.value();
    auto pad_tag = params.GetTagValue(TAG_PADDING);
    if (pad_tag.has_value()) padding = pad_tag.value();
    auto mgf_tag = params.GetTagValue(TAG_RSA_OAEP_MGF_DIGEST);
    if (mgf_tag.has_value()) mgf_digest = mgf_tag.value();

    const EVP_MD* md = openssl_digest(digest);
    const EVP_MD* mgf_md = openssl_digest(mgf_digest);

    // Set up encryption context.
    EVP_PKEY_CTX_Ptr ctx(EVP_PKEY_CTX_new(pub_key.get(), /* engine= */ nullptr));
    if (EVP_PKEY_encrypt_init(ctx.get()) <= 0) {
        ADD_FAILURE() << "Encryption init failed: " << ERR_peek_last_error();
        return "Failure";
    }

    int rc = -1;
    switch (padding) {
        case PaddingMode::NONE:
            rc = EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_NO_PADDING);
            break;
        case PaddingMode::RSA_PKCS1_1_5_ENCRYPT:
            rc = EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING);
            break;
        case PaddingMode::RSA_OAEP:
            rc = EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING);
            break;
        default:
            break;
    }
    if (rc <= 0) {
        ADD_FAILURE() << "Set padding failed: " << ERR_peek_last_error();
        return "Failure";
    }
    if (padding == PaddingMode::RSA_OAEP) {
        if (!EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), md)) {
            ADD_FAILURE() << "Set digest failed: " << ERR_peek_last_error();
            return "Failure";
        }
        if (!EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), mgf_md)) {
            ADD_FAILURE() << "Set MGF digest failed: " << ERR_peek_last_error();
            return "Failure";
        }
    }

    // Determine output size.
    size_t outlen;
    if (EVP_PKEY_encrypt(ctx.get(), nullptr /* out */, &outlen,
                         reinterpret_cast<const uint8_t*>(message.data()), message.size()) <= 0) {
        ADD_FAILURE() << "Determine output size failed: " << ERR_peek_last_error();
        return "Failure";
    }

    // Left-zero-pad the input if necessary.
    const uint8_t* to_encrypt = reinterpret_cast<const uint8_t*>(message.data());
    size_t to_encrypt_len = message.size();

    std::unique_ptr<string> zero_padded_message;
    if (padding == PaddingMode::NONE && to_encrypt_len < outlen) {
        zero_padded_message.reset(new string(outlen, '\0'));
        memcpy(zero_padded_message->data() + (outlen - to_encrypt_len), message.data(),
               message.size());
        to_encrypt = reinterpret_cast<const uint8_t*>(zero_padded_message->data());
        to_encrypt_len = outlen;
    }

    // Do the encryption.
    string output(outlen, '\0');
    if (EVP_PKEY_encrypt(ctx.get(), reinterpret_cast<uint8_t*>(output.data()), &outlen, to_encrypt,
                         to_encrypt_len) <= 0) {
        ADD_FAILURE() << "Encryption failed: " << ERR_peek_last_error();
        return "Failure";
    }
    return output;
}

string KeyMintAidlTestBase::EncryptMessage(const vector<uint8_t>& key_blob, const string& message,
                                           const AuthorizationSet& in_params,
                                           AuthorizationSet* out_params) {
    SCOPED_TRACE("EncryptMessage");
    return ProcessMessage(key_blob, KeyPurpose::ENCRYPT, message, in_params, out_params);
}

string KeyMintAidlTestBase::EncryptMessage(const string& message, const AuthorizationSet& params,
                                           AuthorizationSet* out_params) {
    SCOPED_TRACE("EncryptMessage");
    return EncryptMessage(key_blob_, message, params, out_params);
}

string KeyMintAidlTestBase::EncryptMessage(const string& message, const AuthorizationSet& params) {
    SCOPED_TRACE("EncryptMessage");
    AuthorizationSet out_params;
    string ciphertext = EncryptMessage(message, params, &out_params);
    EXPECT_TRUE(out_params.empty()) << "Output params should be empty. Contained: " << out_params;
    return ciphertext;
}

string KeyMintAidlTestBase::EncryptMessage(const string& message, BlockMode block_mode,
                                           PaddingMode padding) {
    SCOPED_TRACE("EncryptMessage");
    auto params = AuthorizationSetBuilder().BlockMode(block_mode).Padding(padding);
    AuthorizationSet out_params;
    string ciphertext = EncryptMessage(message, params, &out_params);
    EXPECT_TRUE(out_params.empty()) << "Output params should be empty. Contained: " << out_params;
    return ciphertext;
}

string KeyMintAidlTestBase::EncryptMessage(const string& message, BlockMode block_mode,
                                           PaddingMode padding, vector<uint8_t>* iv_out) {
    SCOPED_TRACE("EncryptMessage");
    auto params = AuthorizationSetBuilder().BlockMode(block_mode).Padding(padding);
    AuthorizationSet out_params;
    string ciphertext = EncryptMessage(message, params, &out_params);
    EXPECT_EQ(1U, out_params.size());
    auto ivVal = out_params.GetTagValue(TAG_NONCE);
    EXPECT_TRUE(ivVal);
    if (ivVal) *iv_out = *ivVal;
    return ciphertext;
}

string KeyMintAidlTestBase::EncryptMessage(const string& message, BlockMode block_mode,
                                           PaddingMode padding, const vector<uint8_t>& iv_in) {
    SCOPED_TRACE("EncryptMessage");
    auto params = AuthorizationSetBuilder()
                          .BlockMode(block_mode)
                          .Padding(padding)
                          .Authorization(TAG_NONCE, iv_in);
    AuthorizationSet out_params;
    string ciphertext = EncryptMessage(message, params, &out_params);
    return ciphertext;
}

string KeyMintAidlTestBase::EncryptMessage(const string& message, BlockMode block_mode,
                                           PaddingMode padding, uint8_t mac_length_bits,
                                           const vector<uint8_t>& iv_in) {
    SCOPED_TRACE("EncryptMessage");
    auto params = AuthorizationSetBuilder()
                          .BlockMode(block_mode)
                          .Padding(padding)
                          .Authorization(TAG_MAC_LENGTH, mac_length_bits)
                          .Authorization(TAG_NONCE, iv_in);
    AuthorizationSet out_params;
    string ciphertext = EncryptMessage(message, params, &out_params);
    return ciphertext;
}

string KeyMintAidlTestBase::EncryptMessage(const string& message, BlockMode block_mode,
                                           PaddingMode padding, uint8_t mac_length_bits) {
    SCOPED_TRACE("EncryptMessage");
    auto params = AuthorizationSetBuilder()
                          .BlockMode(block_mode)
                          .Padding(padding)
                          .Authorization(TAG_MAC_LENGTH, mac_length_bits);
    AuthorizationSet out_params;
    string ciphertext = EncryptMessage(message, params, &out_params);
    return ciphertext;
}

string KeyMintAidlTestBase::DecryptMessage(const vector<uint8_t>& key_blob,
                                           const string& ciphertext,
                                           const AuthorizationSet& params) {
    SCOPED_TRACE("DecryptMessage");
    AuthorizationSet out_params;
    string plaintext =
            ProcessMessage(key_blob, KeyPurpose::DECRYPT, ciphertext, params, &out_params);
    EXPECT_TRUE(out_params.empty());
    return plaintext;
}

string KeyMintAidlTestBase::DecryptMessage(const string& ciphertext,
                                           const AuthorizationSet& params) {
    SCOPED_TRACE("DecryptMessage");
    return DecryptMessage(key_blob_, ciphertext, params);
}

string KeyMintAidlTestBase::DecryptMessage(const string& ciphertext, BlockMode block_mode,
                                           PaddingMode padding_mode, const vector<uint8_t>& iv) {
    SCOPED_TRACE("DecryptMessage");
    auto params = AuthorizationSetBuilder()
                          .BlockMode(block_mode)
                          .Padding(padding_mode)
                          .Authorization(TAG_NONCE, iv);
    return DecryptMessage(key_blob_, ciphertext, params);
}

std::pair<ErrorCode, vector<uint8_t>> KeyMintAidlTestBase::UpgradeKey(
        const vector<uint8_t>& key_blob) {
    std::pair<ErrorCode, vector<uint8_t>> retval;
    vector<uint8_t> outKeyBlob;
    Status result = keymint_->upgradeKey(key_blob, vector<KeyParameter>(), &outKeyBlob);
    ErrorCode errorcode = GetReturnErrorCode(result);
    retval = std::tie(errorcode, outKeyBlob);

    return retval;
}

bool KeyMintAidlTestBase::IsRkpSupportRequired() const {
    // This is technically weaker than the VSR-12 requirements, but when
    // Android 12 shipped, there was a bug that skipped the tests if KeyMint
    // 2 was not present. As a result, many chipsets were allowed to ship
    // without RKP support. The RKP requirements were hardened in VSR-13.
    return get_vendor_api_level() >= __ANDROID_API_T__;
}

vector<uint32_t> KeyMintAidlTestBase::ValidKeySizes(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::RSA:
            switch (SecLevel()) {
                case SecurityLevel::SOFTWARE:
                case SecurityLevel::TRUSTED_ENVIRONMENT:
                    return {2048, 3072, 4096};
                case SecurityLevel::STRONGBOX:
                    return {2048};
                default:
                    ADD_FAILURE() << "Invalid security level " << uint32_t(SecLevel());
                    break;
            }
            break;
        case Algorithm::EC:
            ADD_FAILURE() << "EC keys must be specified by curve not size";
            break;
        case Algorithm::AES:
            return {128, 256};
        case Algorithm::TRIPLE_DES:
            return {168};
        case Algorithm::HMAC: {
            vector<uint32_t> retval((512 - 64) / 8 + 1);
            uint32_t size = 64 - 8;
            std::generate(retval.begin(), retval.end(), [&]() { return (size += 8); });
            return retval;
        }
        default:
            ADD_FAILURE() << "Invalid Algorithm: " << algorithm;
            return {};
    }
    ADD_FAILURE() << "Should be impossible to get here";
    return {};
}

vector<uint32_t> KeyMintAidlTestBase::InvalidKeySizes(Algorithm algorithm) {
    if (SecLevel() == SecurityLevel::STRONGBOX) {
        switch (algorithm) {
            case Algorithm::RSA:
                return {3072, 4096};
            case Algorithm::EC:
                return {224, 384, 521};
            case Algorithm::AES:
                return {192};
            case Algorithm::TRIPLE_DES:
                return {56};
            default:
                return {};
        }
    } else {
        switch (algorithm) {
            case Algorithm::AES:
                return {64, 96, 131, 512};
            case Algorithm::TRIPLE_DES:
                return {56};
            default:
                return {};
        }
    }
    return {};
}

vector<BlockMode> KeyMintAidlTestBase::ValidBlockModes(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::AES:
            return {
                    BlockMode::CBC,
                    BlockMode::CTR,
                    BlockMode::ECB,
                    BlockMode::GCM,
            };
        case Algorithm::TRIPLE_DES:
            return {
                    BlockMode::CBC,
                    BlockMode::ECB,
            };
        default:
            return {};
    }
}

vector<PaddingMode> KeyMintAidlTestBase::ValidPaddingModes(Algorithm algorithm,
                                                           BlockMode blockMode) {
    switch (algorithm) {
        case Algorithm::AES:
            switch (blockMode) {
                case BlockMode::CBC:
                case BlockMode::ECB:
                    return {PaddingMode::NONE, PaddingMode::PKCS7};
                case BlockMode::CTR:
                case BlockMode::GCM:
                    return {PaddingMode::NONE};
                default:
                    return {};
            };
        case Algorithm::TRIPLE_DES:
            switch (blockMode) {
                case BlockMode::CBC:
                case BlockMode::ECB:
                    return {PaddingMode::NONE, PaddingMode::PKCS7};
                default:
                    return {};
            };
        default:
            return {};
    }
}

vector<PaddingMode> KeyMintAidlTestBase::InvalidPaddingModes(Algorithm algorithm,
                                                             BlockMode blockMode) {
    switch (algorithm) {
        case Algorithm::AES:
            switch (blockMode) {
                case BlockMode::CTR:
                case BlockMode::GCM:
                    return {PaddingMode::PKCS7};
                default:
                    return {};
            };
        default:
            return {};
    }
}

vector<EcCurve> KeyMintAidlTestBase::ValidCurves() {
    if (securityLevel_ == SecurityLevel::STRONGBOX) {
        return {EcCurve::P_256};
    } else if (Curve25519Supported()) {
        return {EcCurve::P_224, EcCurve::P_256, EcCurve::P_384, EcCurve::P_521,
                EcCurve::CURVE_25519};
    } else {
        return {
                EcCurve::P_224,
                EcCurve::P_256,
                EcCurve::P_384,
                EcCurve::P_521,
        };
    }
}

vector<EcCurve> KeyMintAidlTestBase::InvalidCurves() {
    if (SecLevel() == SecurityLevel::STRONGBOX) {
        // Curve 25519 is not supported, either because:
        // - KeyMint v1: it's an unknown enum value
        // - KeyMint v2+: it's not supported by StrongBox.
        return {EcCurve::P_224, EcCurve::P_384, EcCurve::P_521, EcCurve::CURVE_25519};
    } else {
        if (Curve25519Supported()) {
            return {};
        } else {
            return {EcCurve::CURVE_25519};
        }
    }
}

vector<uint64_t> KeyMintAidlTestBase::ValidExponents() {
    if (SecLevel() == SecurityLevel::STRONGBOX) {
        return {65537};
    } else {
        return {3, 65537};
    }
}

vector<Digest> KeyMintAidlTestBase::ValidDigests(bool withNone, bool withMD5) {
    switch (SecLevel()) {
        case SecurityLevel::SOFTWARE:
        case SecurityLevel::TRUSTED_ENVIRONMENT:
            if (withNone) {
                if (withMD5)
                    return {Digest::NONE,      Digest::MD5,       Digest::SHA1,
                            Digest::SHA_2_224, Digest::SHA_2_256, Digest::SHA_2_384,
                            Digest::SHA_2_512};
                else
                    return {Digest::NONE,      Digest::SHA1,      Digest::SHA_2_224,
                            Digest::SHA_2_256, Digest::SHA_2_384, Digest::SHA_2_512};
            } else {
                if (withMD5)
                    return {Digest::MD5,       Digest::SHA1,      Digest::SHA_2_224,
                            Digest::SHA_2_256, Digest::SHA_2_384, Digest::SHA_2_512};
                else
                    return {Digest::SHA1, Digest::SHA_2_224, Digest::SHA_2_256, Digest::SHA_2_384,
                            Digest::SHA_2_512};
            }
            break;
        case SecurityLevel::STRONGBOX:
            if (withNone)
                return {Digest::NONE, Digest::SHA_2_256};
            else
                return {Digest::SHA_2_256};
            break;
        default:
            ADD_FAILURE() << "Invalid security level " << uint32_t(SecLevel());
            break;
    }
    ADD_FAILURE() << "Should be impossible to get here";
    return {};
}

static const vector<KeyParameter> kEmptyAuthList{};

const vector<KeyParameter>& KeyMintAidlTestBase::SecLevelAuthorizations(
        const vector<KeyCharacteristics>& key_characteristics) {
    auto found = std::find_if(key_characteristics.begin(), key_characteristics.end(),
                              [this](auto& entry) { return entry.securityLevel == SecLevel(); });
    return (found == key_characteristics.end()) ? kEmptyAuthList : found->authorizations;
}

const vector<KeyParameter>& KeyMintAidlTestBase::SecLevelAuthorizations(
        const vector<KeyCharacteristics>& key_characteristics, SecurityLevel securityLevel) {
    auto found = std::find_if(
            key_characteristics.begin(), key_characteristics.end(),
            [securityLevel](auto& entry) { return entry.securityLevel == securityLevel; });
    return (found == key_characteristics.end()) ? kEmptyAuthList : found->authorizations;
}

ErrorCode KeyMintAidlTestBase::UseAesKey(const vector<uint8_t>& aesKeyBlob) {
    auto [result, ciphertext] = ProcessMessage(
            aesKeyBlob, KeyPurpose::ENCRYPT, "1234567890123456",
            AuthorizationSetBuilder().BlockMode(BlockMode::ECB).Padding(PaddingMode::NONE));
    return result;
}

ErrorCode KeyMintAidlTestBase::UseHmacKey(const vector<uint8_t>& hmacKeyBlob) {
    auto [result, mac] = ProcessMessage(
            hmacKeyBlob, KeyPurpose::SIGN, "1234567890123456",
            AuthorizationSetBuilder().Authorization(TAG_MAC_LENGTH, 128).Digest(Digest::SHA_2_256));
    return result;
}

ErrorCode KeyMintAidlTestBase::UseRsaKey(const vector<uint8_t>& rsaKeyBlob) {
    std::string message(2048 / 8, 'a');
    auto [result, signature] = ProcessMessage(
            rsaKeyBlob, KeyPurpose::SIGN, message,
            AuthorizationSetBuilder().Digest(Digest::NONE).Padding(PaddingMode::NONE));
    return result;
}

ErrorCode KeyMintAidlTestBase::UseEcdsaKey(const vector<uint8_t>& ecdsaKeyBlob) {
    auto [result, signature] = ProcessMessage(ecdsaKeyBlob, KeyPurpose::SIGN, "a",
                                              AuthorizationSetBuilder().Digest(Digest::SHA_2_256));
    return result;
}

ErrorCode KeyMintAidlTestBase::GenerateAttestKey(const AuthorizationSet& key_desc,
                                                 const optional<AttestationKey>& attest_key,
                                                 vector<uint8_t>* key_blob,
                                                 vector<KeyCharacteristics>* key_characteristics,
                                                 vector<Certificate>* cert_chain) {
    // The original specification for KeyMint v1 (introduced in Android 12) required ATTEST_KEY not
    // be combined with any other key purpose, but the original VTS-12 tests incorrectly did exactly
    // that. The tests were fixed in VTS-13 (vendor API level 33). This means that devices with
    // vendor API level < 33 may accept or even require KeyPurpose::SIGN too.
    if (get_vendor_api_level() < __ANDROID_API_T__) {
        AuthorizationSet key_desc_plus_sign = key_desc;
        key_desc_plus_sign.push_back(TAG_PURPOSE, KeyPurpose::SIGN);

        auto result = GenerateKey(key_desc_plus_sign, attest_key, key_blob, key_characteristics,
                                  cert_chain);
        if (result == ErrorCode::OK) {
            return result;
        }
        // If the key generation failed, it may be because the device is (correctly)
        // rejecting the combination of ATTEST_KEY+SIGN.  Fall through to try again with
        // just ATTEST_KEY.
    }
    return GenerateKey(key_desc, attest_key, key_blob, key_characteristics, cert_chain);
}

// Check if ATTEST_KEY feature is disabled
bool KeyMintAidlTestBase::is_attest_key_feature_disabled(void) const {
    if (!check_feature(FEATURE_KEYSTORE_APP_ATTEST_KEY)) {
        GTEST_LOG_(INFO) << "Feature " + FEATURE_KEYSTORE_APP_ATTEST_KEY + " is disabled";
        return true;
    }

    return false;
}

// Check if StrongBox KeyStore is enabled
bool KeyMintAidlTestBase::is_strongbox_enabled(void) const {
    if (check_feature(FEATURE_STRONGBOX_KEYSTORE)) {
        GTEST_LOG_(INFO) << "Feature " + FEATURE_STRONGBOX_KEYSTORE + " is enabled";
        return true;
    }

    return false;
}

// Check if chipset has received a waiver allowing it to be launched with Android S or T with
// Keymaster 4.0 in StrongBox.
bool KeyMintAidlTestBase::is_chipset_allowed_km4_strongbox(void) const {
    std::array<char, PROPERTY_VALUE_MAX> buffer;

    const int32_t first_api_level = property_get_int32("ro.board.first_api_level", 0);
    if (first_api_level <= 0 || first_api_level > __ANDROID_API_T__) return false;

    auto res = property_get("ro.vendor.qti.soc_model", buffer.data(), nullptr);
    if (res <= 0) return false;

    const string allowed_soc_models[] = {"SM8450", "SM8475", "SM8550", "SXR2230P",
                                         "SM4450", "SM7450", "SM6450"};

    for (const string model : allowed_soc_models) {
        if (model.compare(buffer.data()) == 0) {
            GTEST_LOG_(INFO) << "QTI SOC Model " + model + " is allowed SB KM 4.0";
            return true;
        }
    }

    return false;
}

// Indicate whether a test that involves use of the ATTEST_KEY feature should be
// skipped.
//
// In general, every KeyMint implementation should support ATTEST_KEY;
// however, there is a waiver for some specific devices that ship with a
// combination of Keymaster/StrongBox and KeyMint/TEE.  On these devices, the
// ATTEST_KEY feature is disabled in the KeyMint/TEE implementation so that
// the device has consistent ATTEST_KEY behavior (ie. UNIMPLEMENTED) across both
// HAL implementations.
//
// This means that a test involving ATTEST_KEY test should be skipped if all of
// the following conditions hold:
// 1. The device is running one of the chipsets that have received a waiver
//     allowing it to be launched with Android S or T with Keymaster 4.0
//     in StrongBox
// 2. The device has a STRONGBOX implementation present.
// 3. ATTEST_KEY feature is advertised as disabled.
//
// Note that in this scenario, ATTEST_KEY tests should be skipped for both
// the StrongBox implementation (which is Keymaster, therefore not tested here)
// and for the TEE implementation (which is adjusted to return UNIMPLEMENTED
// specifically for this waiver).
bool KeyMintAidlTestBase::shouldSkipAttestKeyTest(void) const {
    // Check the chipset first as that doesn't require a round-trip to Package Manager.
    return (is_chipset_allowed_km4_strongbox() && is_strongbox_enabled() &&
            is_attest_key_feature_disabled());
}

void verify_serial(X509* cert, const uint64_t expected_serial) {
    BIGNUM_Ptr ser(BN_new());
    EXPECT_TRUE(ASN1_INTEGER_to_BN(X509_get_serialNumber(cert), ser.get()));

    uint64_t serial;
    EXPECT_TRUE(BN_get_u64(ser.get(), &serial));
    EXPECT_EQ(serial, expected_serial);
}

// Please set self_signed to true for fake certificates or self signed
// certificates
void verify_subject(const X509* cert,       //
                    const string& subject,  //
                    bool self_signed) {
    char* cert_issuer =  //
            X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);

    char* cert_subj = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);

    string expected_subject("/CN=");
    if (subject.empty()) {
        expected_subject.append("Android Keystore Key");
    } else {
        expected_subject.append(subject);
    }

    EXPECT_STREQ(expected_subject.c_str(), cert_subj) << "Cert has wrong subject." << cert_subj;

    if (self_signed) {
        EXPECT_STREQ(cert_issuer, cert_subj)
                << "Cert issuer and subject mismatch for self signed certificate.";
    }

    OPENSSL_free(cert_subj);
    OPENSSL_free(cert_issuer);
}

int get_vendor_api_level() {
    // Android 13+ builds have the `ro.vendor.api_level` system property. See
    // https://source.android.com/docs/core/architecture/api-flags#determine_vendor_api_level_android_13.
    int vendor_api_level = ::android::base::GetIntProperty("ro.vendor.api_level", -1);
    if (vendor_api_level != -1) {
        return vendor_api_level;
    }

    // Android 12 builds have the `ro.board.api_level` and `ro.board.first_api_level` system
    // properties, which are only expected to be populated for GRF SoCs on Android 12 builds. Note
    // that they are populated automatically by the build system starting in Android 15, but we use
    // `ro.vendor.api_level` on such builds (see above). For details, see
    // https://docs.partner.android.com/gms/building/integrating/extending-os-upgrade-support-windows#new-system-properties.
    vendor_api_level = ::android::base::GetIntProperty("ro.board.api_level", -1);
    if (vendor_api_level == -1) {
        vendor_api_level = ::android::base::GetIntProperty("ro.board.first_api_level", -1);
    }

    int product_api_level = ::android::base::GetIntProperty("ro.product.first_api_level", -1);
    if (product_api_level == -1) {
        product_api_level = ::android::base::GetIntProperty("ro.build.version.sdk", -1);
        EXPECT_NE(product_api_level, -1) << "Could not find ro.build.version.sdk";
    }

    // If the `ro.board.api_level` and `ro.board.first_api_level` properties aren't populated, it
    // means the build doesn't have a GRF SoC, so the product API level should be used.
    if (vendor_api_level == -1) {
        return product_api_level;
    }
    return std::min(product_api_level, vendor_api_level);
}

bool is_gsi_image() {
    std::ifstream ifs("/system/system_ext/etc/init/init.gsi.rc");
    return ifs.good();
}

vector<uint8_t> build_serial_blob(const uint64_t serial_int) {
    BIGNUM_Ptr serial(BN_new());
    EXPECT_TRUE(BN_set_u64(serial.get(), serial_int));

    int len = BN_num_bytes(serial.get());
    vector<uint8_t> serial_blob(len);
    if (BN_bn2bin(serial.get(), serial_blob.data()) != len) {
        return {};
    }

    if (serial_blob.empty() || serial_blob[0] & 0x80) {
        // An empty blob is OpenSSL's encoding of the zero value; we need single zero byte.
        // Top bit being set indicates a negative number in two's complement, but our input
        // was positive.
        // In either case, prepend a zero byte.
        serial_blob.insert(serial_blob.begin(), 0x00);
    }

    return serial_blob;
}

void verify_subject_and_serial(const Certificate& certificate,  //
                               const uint64_t expected_serial,  //
                               const string& subject, bool self_signed) {
    X509_Ptr cert(parse_cert_blob(certificate.encodedCertificate));
    ASSERT_TRUE(!!cert.get());

    verify_serial(cert.get(), expected_serial);
    verify_subject(cert.get(), subject, self_signed);
}

void verify_root_of_trust(const vector<uint8_t>& verified_boot_key, bool device_locked,
                          VerifiedBoot verified_boot_state,
                          const vector<uint8_t>& verified_boot_hash) {
    char property_value[PROPERTY_VALUE_MAX] = {};

    if (avb_verification_enabled()) {
        EXPECT_NE(property_get("ro.boot.vbmeta.digest", property_value, ""), 0);
        string prop_string(property_value);
        EXPECT_EQ(prop_string.size(), 64);
        EXPECT_EQ(prop_string, bin2hex(verified_boot_hash));

        EXPECT_NE(property_get("ro.boot.vbmeta.device_state", property_value, ""), 0);
        if (!strcmp(property_value, "unlocked")) {
            EXPECT_FALSE(device_locked);
        } else {
            EXPECT_TRUE(device_locked);
        }

        // Check that the device is locked if not debuggable, e.g., user build
        // images in CTS. For VTS, debuggable images are used to allow adb root
        // and the device is unlocked.
        if (!property_get_bool("ro.debuggable", false)) {
            EXPECT_TRUE(device_locked);
        } else {
            EXPECT_FALSE(device_locked);
        }
    }

    if (get_vendor_api_level() > AVendorSupport_getVendorApiLevelOf(__ANDROID_API_V__)) {
        // The Verified Boot key field should be exactly 32 bytes since it
        // contains the SHA-256 hash of the key on locked devices or 32 bytes
        // of zeroes on unlocked devices. This wasn't checked for earlier
        // versions of the KeyMint HAL, so we version-gate the strict check.
        EXPECT_EQ(verified_boot_key.size(), 32);
    } else if (get_vendor_api_level() == AVendorSupport_getVendorApiLevelOf(__ANDROID_API_V__)) {
        // The Verified Boot key field should be:
        //   - Exactly 32 bytes on locked devices since it should contain
        //     the SHA-256 hash of the key, or
        //   - Up to 32 bytes of zeroes on unlocked devices (behaviour on
        //     unlocked devices isn't specified in the HAL interface
        //     specification).
        // Thus, we can't check for strict equality in case unlocked devices
        // report values with less than 32 bytes. This wasn't checked for
        // earlier versions of the KeyMint HAL, so we version-gate the check.
        EXPECT_LE(verified_boot_key.size(), 32);
    }

    // Verified Boot key should be all zeroes if the boot state is "orange".
    std::string empty_boot_key(32, '\0');
    std::string verified_boot_key_str((const char*)verified_boot_key.data(),
                                      verified_boot_key.size());
    EXPECT_NE(property_get("ro.boot.verifiedbootstate", property_value, ""), 0);
    if (!strcmp(property_value, "green")) {
        EXPECT_EQ(verified_boot_state, VerifiedBoot::VERIFIED);
        EXPECT_NE(0, memcmp(verified_boot_key.data(), empty_boot_key.data(),
                            verified_boot_key.size()));
    } else if (!strcmp(property_value, "yellow")) {
        EXPECT_EQ(verified_boot_state, VerifiedBoot::SELF_SIGNED);
        EXPECT_NE(0, memcmp(verified_boot_key.data(), empty_boot_key.data(),
                            verified_boot_key.size()));
    } else if (!strcmp(property_value, "orange")) {
        EXPECT_EQ(verified_boot_state, VerifiedBoot::UNVERIFIED);
        EXPECT_EQ(0, memcmp(verified_boot_key.data(), empty_boot_key.data(),
                            verified_boot_key.size()));
    } else if (!strcmp(property_value, "red")) {
        EXPECT_EQ(verified_boot_state, VerifiedBoot::FAILED);
    } else {
        EXPECT_EQ(verified_boot_state, VerifiedBoot::UNVERIFIED);
        EXPECT_EQ(0, memcmp(verified_boot_key.data(), empty_boot_key.data(),
                            verified_boot_key.size()));
    }
}

bool verify_attestation_record(int32_t aidl_version,                   //
                               const string& challenge,                //
                               const string& app_id,                   //
                               AuthorizationSet expected_sw_enforced,  //
                               AuthorizationSet expected_hw_enforced,  //
                               SecurityLevel security_level,
                               const vector<uint8_t>& attestation_cert,
                               vector<uint8_t>* unique_id) {
    X509_Ptr cert(parse_cert_blob(attestation_cert));
    EXPECT_TRUE(!!cert.get());
    if (!cert.get()) return false;

    // Make sure CRL Distribution Points extension is not present in a certificate
    // containing attestation record.
    check_crl_distribution_points_extension_not_present(cert.get());

    ASN1_OCTET_STRING* attest_rec = get_attestation_record(cert.get());
    EXPECT_TRUE(!!attest_rec);
    if (!attest_rec) return false;

    AuthorizationSet att_sw_enforced;
    AuthorizationSet att_hw_enforced;
    uint32_t att_attestation_version;
    uint32_t att_keymint_version;
    SecurityLevel att_attestation_security_level;
    SecurityLevel att_keymint_security_level;
    vector<uint8_t> att_challenge;
    vector<uint8_t> att_unique_id;
    vector<uint8_t> att_app_id;

    auto error = parse_attestation_record(attest_rec->data,                 //
                                          attest_rec->length,               //
                                          &att_attestation_version,         //
                                          &att_attestation_security_level,  //
                                          &att_keymint_version,             //
                                          &att_keymint_security_level,      //
                                          &att_challenge,                   //
                                          &att_sw_enforced,                 //
                                          &att_hw_enforced,                 //
                                          &att_unique_id);
    EXPECT_EQ(ErrorCode::OK, error);
    if (error != ErrorCode::OK) return false;

    check_attestation_version(att_attestation_version, aidl_version);
    vector<uint8_t> appId(app_id.begin(), app_id.end());

    // check challenge and app id only if we expects a non-fake certificate
    if (challenge.length() > 0) {
        EXPECT_EQ(challenge.length(), att_challenge.size());
        EXPECT_EQ(0, memcmp(challenge.data(), att_challenge.data(), challenge.length()));

        expected_sw_enforced.push_back(TAG_ATTESTATION_APPLICATION_ID, appId);
    }

    check_attestation_version(att_keymint_version, aidl_version);
    EXPECT_EQ(security_level, att_keymint_security_level);
    EXPECT_EQ(security_level, att_attestation_security_level);

    for (int i = 0; i < att_hw_enforced.size(); i++) {
        if (att_hw_enforced[i].tag == TAG_BOOT_PATCHLEVEL ||
            att_hw_enforced[i].tag == TAG_VENDOR_PATCHLEVEL) {
            std::string date =
                    std::to_string(att_hw_enforced[i].value.get<KeyParameterValue::integer>());

            // strptime seems to require delimiters, but the tag value will
            // be YYYYMMDD
            if (date.size() != 8) {
                ADD_FAILURE() << "Tag " << att_hw_enforced[i].tag
                              << " with invalid format (not YYYYMMDD): " << date;
                return false;
            }
            date.insert(6, "-");
            date.insert(4, "-");
            struct tm time;
            strptime(date.c_str(), "%Y-%m-%d", &time);

            // Day of the month (0-31)
            EXPECT_GE(time.tm_mday, 0);
            EXPECT_LT(time.tm_mday, 32);
            // Months since Jan (0-11)
            EXPECT_GE(time.tm_mon, 0);
            EXPECT_LT(time.tm_mon, 12);
            // Years since 1900
            EXPECT_GT(time.tm_year, 110);
            EXPECT_LT(time.tm_year, 200);
        }
    }

    // Check to make sure boolean values are properly encoded. Presence of a boolean tag
    // indicates true. A provided boolean tag that can be pulled back out of the certificate
    // indicates correct encoding. No need to check if it's in both lists, since the
    // AuthorizationSet compare below will handle mismatches of tags.
    if (security_level == SecurityLevel::SOFTWARE) {
        EXPECT_TRUE(expected_sw_enforced.Contains(TAG_NO_AUTH_REQUIRED));
    } else {
        EXPECT_TRUE(expected_hw_enforced.Contains(TAG_NO_AUTH_REQUIRED));
    }

    if (att_hw_enforced.Contains(TAG_ALGORITHM, Algorithm::EC)) {
        // For ECDSA keys, either an EC_CURVE or a KEY_SIZE can be specified, but one must be.
        EXPECT_TRUE(att_hw_enforced.Contains(TAG_EC_CURVE) ||
                    att_hw_enforced.Contains(TAG_KEY_SIZE));
    }

    // Test root of trust elements
    vector<uint8_t> verified_boot_key;
    VerifiedBoot verified_boot_state;
    bool device_locked;
    vector<uint8_t> verified_boot_hash;
    error = parse_root_of_trust(attest_rec->data, attest_rec->length, &verified_boot_key,
                                &verified_boot_state, &device_locked, &verified_boot_hash);
    EXPECT_EQ(ErrorCode::OK, error);
    verify_root_of_trust(verified_boot_key, device_locked, verified_boot_state, verified_boot_hash);

    att_sw_enforced.Sort();
    expected_sw_enforced.Sort();
    EXPECT_EQ(filtered_tags(expected_sw_enforced), filtered_tags(att_sw_enforced));

    att_hw_enforced.Sort();
    expected_hw_enforced.Sort();
    EXPECT_EQ(filtered_tags(expected_hw_enforced), filtered_tags(att_hw_enforced));

    if (unique_id != nullptr) {
        *unique_id = att_unique_id;
    }

    return true;
}

string bin2hex(const vector<uint8_t>& data) {
    string retval;
    retval.reserve(data.size() * 2 + 1);
    for (uint8_t byte : data) {
        retval.push_back(nibble2hex[0x0F & (byte >> 4)]);
        retval.push_back(nibble2hex[0x0F & byte]);
    }
    return retval;
}

AuthorizationSet HwEnforcedAuthorizations(const vector<KeyCharacteristics>& key_characteristics) {
    AuthorizationSet authList;
    for (auto& entry : key_characteristics) {
        if (entry.securityLevel == SecurityLevel::STRONGBOX ||
            entry.securityLevel == SecurityLevel::TRUSTED_ENVIRONMENT) {
            authList.push_back(AuthorizationSet(entry.authorizations));
        }
    }
    return authList;
}

AuthorizationSet SwEnforcedAuthorizations(const vector<KeyCharacteristics>& key_characteristics) {
    AuthorizationSet authList;
    for (auto& entry : key_characteristics) {
        if (entry.securityLevel == SecurityLevel::SOFTWARE ||
            entry.securityLevel == SecurityLevel::KEYSTORE) {
            authList.push_back(AuthorizationSet(entry.authorizations));
        }
    }
    return authList;
}

AssertionResult ChainSignaturesAreValid(const vector<Certificate>& chain,
                                        bool strict_issuer_check) {
    std::stringstream cert_data;

    for (size_t i = 0; i < chain.size(); ++i) {
        cert_data << bin2hex(chain[i].encodedCertificate) << std::endl;

        X509_Ptr key_cert(parse_cert_blob(chain[i].encodedCertificate));
        X509_Ptr signing_cert;
        if (i < chain.size() - 1) {
            signing_cert = parse_cert_blob(chain[i + 1].encodedCertificate);
        } else {
            signing_cert = parse_cert_blob(chain[i].encodedCertificate);
        }
        if (!key_cert.get() || !signing_cert.get()) return AssertionFailure() << cert_data.str();

        EVP_PKEY_Ptr signing_pubkey(X509_get_pubkey(signing_cert.get()));
        if (!signing_pubkey.get()) return AssertionFailure() << cert_data.str();

        if (!X509_verify(key_cert.get(), signing_pubkey.get())) {
            return AssertionFailure()
                   << "Verification of certificate " << i << " failed "
                   << "OpenSSL error string: " << ERR_error_string(ERR_get_error(), NULL) << '\n'
                   << cert_data.str();
        }

        string cert_issuer = x509NameToStr(X509_get_issuer_name(key_cert.get()));
        string signer_subj = x509NameToStr(X509_get_subject_name(signing_cert.get()));
        if (cert_issuer != signer_subj && strict_issuer_check) {
            return AssertionFailure() << "Cert " << i << " has wrong issuer.\n"
                                      << " Signer subject is " << signer_subj
                                      << " Issuer subject is " << cert_issuer << endl
                                      << cert_data.str();
        }
    }

    if (KeyMintAidlTestBase::dump_Attestations) std::cout << "cert chain:\n" << cert_data.str();
    return AssertionSuccess();
}

ErrorCode GetReturnErrorCode(const Status& result) {
    if (result.isOk()) return ErrorCode::OK;

    if (result.getExceptionCode() == EX_SERVICE_SPECIFIC) {
        return static_cast<ErrorCode>(result.getServiceSpecificError());
    }

    return ErrorCode::UNKNOWN_ERROR;
}

X509_Ptr parse_cert_blob(const vector<uint8_t>& blob) {
    const uint8_t* p = blob.data();
    return X509_Ptr(d2i_X509(nullptr /* allocate new */, &p, blob.size()));
}

// Extract attestation record from cert. Returned object is still part of cert; don't free it
// separately.
ASN1_OCTET_STRING* get_attestation_record(X509* certificate) {
    ASN1_OBJECT_Ptr oid(OBJ_txt2obj(kAttestionRecordOid, 1 /* dotted string format */));
    EXPECT_TRUE(!!oid.get());
    if (!oid.get()) return nullptr;

    int location = X509_get_ext_by_OBJ(certificate, oid.get(), -1 /* search from beginning */);
    EXPECT_NE(-1, location) << "Attestation extension not found in certificate";
    if (location == -1) return nullptr;

    X509_EXTENSION* attest_rec_ext = X509_get_ext(certificate, location);
    EXPECT_TRUE(!!attest_rec_ext)
            << "Found attestation extension but couldn't retrieve it?  Probably a BoringSSL bug.";
    if (!attest_rec_ext) return nullptr;

    ASN1_OCTET_STRING* attest_rec = X509_EXTENSION_get_data(attest_rec_ext);
    EXPECT_TRUE(!!attest_rec) << "Attestation extension contained no data";
    return attest_rec;
}

vector<uint8_t> make_name_from_str(const string& name) {
    X509_NAME_Ptr x509_name(X509_NAME_new());
    EXPECT_TRUE(x509_name.get() != nullptr);
    if (!x509_name) return {};

    EXPECT_EQ(1, X509_NAME_add_entry_by_txt(x509_name.get(),  //
                                            "CN",             //
                                            MBSTRING_ASC,
                                            reinterpret_cast<const uint8_t*>(name.c_str()),
                                            -1,  // len
                                            -1,  // loc
                                            0 /* set */));

    int len = i2d_X509_NAME(x509_name.get(), nullptr /* only return length */);
    EXPECT_GT(len, 0);

    vector<uint8_t> retval(len);
    uint8_t* p = retval.data();
    i2d_X509_NAME(x509_name.get(), &p);

    return retval;
}

void KeyMintAidlTestBase::assert_mgf_digests_present_or_not_in_key_characteristics(
        std::vector<android::hardware::security::keymint::Digest>& expected_mgf_digests,
        bool is_mgf_digest_expected) const {
    assert_mgf_digests_present_or_not_in_key_characteristics(
            key_characteristics_, expected_mgf_digests, is_mgf_digest_expected);
}

void KeyMintAidlTestBase::assert_mgf_digests_present_or_not_in_key_characteristics(
        const vector<KeyCharacteristics>& key_characteristics,
        std::vector<android::hardware::security::keymint::Digest>& expected_mgf_digests,
        bool is_mgf_digest_expected) const {
    // There was no test to assert that MGF1 digest was present in generated/imported key
    // characteristics before Keymint V3, so there are some Keymint implementations where
    // asserting for MGF1 digest fails(b/297306437), hence skipping for Keymint < 3.
    if (AidlVersion() < 3) {
        return;
    }
    AuthorizationSet auths;
    for (auto& entry : key_characteristics) {
        auths.push_back(AuthorizationSet(entry.authorizations));
    }
    for (auto digest : expected_mgf_digests) {
        if (is_mgf_digest_expected) {
            ASSERT_TRUE(auths.Contains(TAG_RSA_OAEP_MGF_DIGEST, digest));
        } else {
            ASSERT_FALSE(auths.Contains(TAG_RSA_OAEP_MGF_DIGEST, digest));
        }
    }
}

namespace {

std::optional<std::string> validateP256Point(const std::vector<uint8_t>& x_buffer,
                                             const std::vector<uint8_t>& y_buffer) {
    auto group = EC_GROUP_Ptr(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
    if (group.get() == nullptr) {
        return "Error creating EC group by curve name for prime256v1";
    }

    auto point = EC_POINT_Ptr(EC_POINT_new(group.get()));
    BIGNUM_Ptr x(BN_bin2bn(x_buffer.data(), x_buffer.size(), nullptr));
    BIGNUM_Ptr y(BN_bin2bn(y_buffer.data(), y_buffer.size(), nullptr));
    if (!EC_POINT_set_affine_coordinates_GFp(group.get(), point.get(), x.get(), y.get(), nullptr)) {
        return "Failed to set affine coordinates.";
    }
    if (!EC_POINT_is_on_curve(group.get(), point.get(), nullptr)) {
        return "Point is not on curve.";
    }
    if (EC_POINT_is_at_infinity(group.get(), point.get())) {
        return "Point is at infinity.";
    }
    const auto* generator = EC_GROUP_get0_generator(group.get());
    if (!EC_POINT_cmp(group.get(), generator, point.get(), nullptr)) {
        return "Point is equal to generator.";
    }

    return std::nullopt;
}

void check_cose_key(const vector<uint8_t>& data, bool testMode) {
    auto [parsedPayload, __, payloadParseErr] = cppbor::parse(data);
    ASSERT_TRUE(parsedPayload) << "Key parse failed: " << payloadParseErr;

    // The following check assumes that canonical CBOR encoding is used for the COSE_Key.
    if (testMode) {
        EXPECT_THAT(
                cppbor::prettyPrint(parsedPayload.get()),
                MatchesRegex("\\{\n"
                             "  1 : 2,\n"   // kty: EC2
                             "  3 : -7,\n"  // alg: ES256
                             "  -1 : 1,\n"  // EC id: P256
                             // The regex {(0x[0-9a-f]{2}, ){31}0x[0-9a-f]{2}} matches a
                             // sequence of 32 hexadecimal bytes, enclosed in braces and
                             // separated by commas. In this case, some Ed25519 public key.
                             "  -2 : \\{(0x[0-9a-f]{2}, ){31}0x[0-9a-f]{2}\\},\n"  // pub_x: data
                             "  -3 : \\{(0x[0-9a-f]{2}, ){31}0x[0-9a-f]{2}\\},\n"  // pub_y: data
                             "  -70000 : null,\n"                                  // test marker
                             "\\}"));
    } else {
        EXPECT_THAT(
                cppbor::prettyPrint(parsedPayload.get()),
                MatchesRegex("\\{\n"
                             "  1 : 2,\n"   // kty: EC2
                             "  3 : -7,\n"  // alg: ES256
                             "  -1 : 1,\n"  // EC id: P256
                             // The regex {(0x[0-9a-f]{2}, ){31}0x[0-9a-f]{2}} matches a
                             // sequence of 32 hexadecimal bytes, enclosed in braces and
                             // separated by commas. In this case, some Ed25519 public key.
                             "  -2 : \\{(0x[0-9a-f]{2}, ){31}0x[0-9a-f]{2}\\},\n"  // pub_x: data
                             "  -3 : \\{(0x[0-9a-f]{2}, ){31}0x[0-9a-f]{2}\\},\n"  // pub_y: data
                             "\\}"));
    }

    ASSERT_TRUE(parsedPayload->asMap()) << "CBOR item was not a map";

    ASSERT_TRUE(parsedPayload->asMap()->get(CoseKey::Label::PUBKEY_X))
            << "CBOR map did not contain x coordinate of public key";
    ASSERT_TRUE(parsedPayload->asMap()->get(CoseKey::Label::PUBKEY_X)->asBstr())
            << "x coordinate of public key was not a bstr";
    const auto& x = parsedPayload->asMap()->get(CoseKey::Label::PUBKEY_X)->asBstr()->value();

    ASSERT_TRUE(parsedPayload->asMap()->get(CoseKey::Label::PUBKEY_Y))
            << "CBOR map did not contain y coordinate of public key";
    ASSERT_TRUE(parsedPayload->asMap()->get(CoseKey::Label::PUBKEY_Y)->asBstr())
            << "y coordinate of public key was not a bstr";
    const auto& y = parsedPayload->asMap()->get(CoseKey::Label::PUBKEY_Y)->asBstr()->value();

    auto errorMessage = validateP256Point(x, y);
    EXPECT_EQ(errorMessage, std::nullopt)
            << *errorMessage << " x: " << bin2hex(x) << " y: " << bin2hex(y);
}

}  // namespace

void check_maced_pubkey(const MacedPublicKey& macedPubKey, bool testMode,
                        vector<uint8_t>* payload_value) {
    auto [coseMac0, _, mac0ParseErr] = cppbor::parse(macedPubKey.macedKey);
    ASSERT_TRUE(coseMac0) << "COSE Mac0 parse failed " << mac0ParseErr;

    ASSERT_NE(coseMac0->asArray(), nullptr);
    ASSERT_EQ(coseMac0->asArray()->size(), kCoseMac0EntryCount);

    auto protParms = coseMac0->asArray()->get(kCoseMac0ProtectedParams)->asBstr();
    ASSERT_NE(protParms, nullptr);

    // Header label:value of 'alg': HMAC-256
    ASSERT_EQ(cppbor::prettyPrint(protParms->value()), "{\n  1 : 5,\n}");

    auto unprotParms = coseMac0->asArray()->get(kCoseMac0UnprotectedParams)->asMap();
    ASSERT_NE(unprotParms, nullptr);
    ASSERT_EQ(unprotParms->size(), 0);

    // The payload is a bstr holding an encoded COSE_Key
    auto payload = coseMac0->asArray()->get(kCoseMac0Payload)->asBstr();
    ASSERT_NE(payload, nullptr);
    check_cose_key(payload->value(), testMode);

    auto coseMac0Tag = coseMac0->asArray()->get(kCoseMac0Tag)->asBstr();
    ASSERT_TRUE(coseMac0Tag);
    auto extractedTag = coseMac0Tag->value();
    EXPECT_EQ(extractedTag.size(), 32U);

    // Compare with tag generated with kTestMacKey.  Should only match in test mode
    auto macFunction = [](const cppcose::bytevec& input) {
        return cppcose::generateHmacSha256(remote_prov::kTestMacKey, input);
    };
    auto testTag =
            cppcose::generateCoseMac0Mac(macFunction, {} /* external_aad */, payload->value());
    ASSERT_TRUE(testTag) << "Tag calculation failed: " << testTag.message();

    if (testMode) {
        EXPECT_THAT(*testTag, ElementsAreArray(extractedTag));
    } else {
        EXPECT_THAT(*testTag, Not(ElementsAreArray(extractedTag)));
    }
    if (payload_value != nullptr) {
        *payload_value = payload->value();
    }
}

void p256_pub_key(const vector<uint8_t>& coseKeyData, EVP_PKEY_Ptr* signingKey) {
    // Extract x and y affine coordinates from the encoded Cose_Key.
    auto [parsedPayload, __, payloadParseErr] = cppbor::parse(coseKeyData);
    ASSERT_TRUE(parsedPayload) << "Key parse failed: " << payloadParseErr;
    auto coseKey = parsedPayload->asMap();
    const std::unique_ptr<cppbor::Item>& xItem = coseKey->get(cppcose::CoseKey::PUBKEY_X);
    ASSERT_NE(xItem->asBstr(), nullptr);
    vector<uint8_t> x = xItem->asBstr()->value();
    const std::unique_ptr<cppbor::Item>& yItem = coseKey->get(cppcose::CoseKey::PUBKEY_Y);
    ASSERT_NE(yItem->asBstr(), nullptr);
    vector<uint8_t> y = yItem->asBstr()->value();

    // Concatenate: 0x04 (uncompressed form marker) | x | y
    vector<uint8_t> pubKeyData{0x04};
    pubKeyData.insert(pubKeyData.end(), x.begin(), x.end());
    pubKeyData.insert(pubKeyData.end(), y.begin(), y.end());

    EC_KEY_Ptr ecKey = EC_KEY_Ptr(EC_KEY_new());
    ASSERT_NE(ecKey, nullptr);
    EC_GROUP_Ptr group = EC_GROUP_Ptr(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(EC_KEY_set_group(ecKey.get(), group.get()), 1);
    EC_POINT_Ptr point = EC_POINT_Ptr(EC_POINT_new(group.get()));
    ASSERT_NE(point, nullptr);
    ASSERT_EQ(EC_POINT_oct2point(group.get(), point.get(), pubKeyData.data(), pubKeyData.size(),
                                 nullptr),
              1);
    ASSERT_EQ(EC_KEY_set_public_key(ecKey.get(), point.get()), 1);

    EVP_PKEY_Ptr pubKey = EVP_PKEY_Ptr(EVP_PKEY_new());
    ASSERT_NE(pubKey, nullptr);
    EVP_PKEY_assign_EC_KEY(pubKey.get(), ecKey.release());
    *signingKey = std::move(pubKey);
}

// Check the error code from an attempt to perform device ID attestation with an invalid value.
void device_id_attestation_check_acceptable_error(Tag tag, const ErrorCode& result) {
    if (result == ErrorCode::CANNOT_ATTEST_IDS) {
        // Standard/default error code for ID mismatch.
    } else if (result == ErrorCode::INVALID_TAG) {
        // Depending on the situation, other error codes may be acceptable.  First, allow older
        // implementations to use INVALID_TAG.
        ASSERT_FALSE(get_vendor_api_level() > __ANDROID_API_T__)
                << "It is a specification violation for INVALID_TAG to be returned due to ID "
                << "mismatch in a Device ID Attestation call. INVALID_TAG is only intended to "
                << "be used for a case where updateAad() is called after update(). As of "
                << "VSR-14, this is now enforced as an error.";
    } else if (result == ErrorCode::ATTESTATION_IDS_NOT_PROVISIONED) {
        // If the device is not a phone, it will not have IMEI/MEID values available.  Allow
        // ATTESTATION_IDS_NOT_PROVISIONED in this case.
        ASSERT_TRUE((tag == TAG_ATTESTATION_ID_IMEI || tag == TAG_ATTESTATION_ID_MEID ||
                     tag == TAG_ATTESTATION_ID_SECOND_IMEI))
                << "incorrect error code on attestation ID mismatch for " << tag;
    } else {
        ADD_FAILURE() << "Error code " << result
                      << " returned on attestation ID mismatch, should be CANNOT_ATTEST_IDS";
    }
}

// Check whether the given named feature is available.
bool check_feature(const std::string& name) {
    ::android::sp<::android::IServiceManager> sm(::android::defaultServiceManager());
    ::android::sp<::android::IBinder> binder(
        sm->waitForService(::android::String16("package_native")));
    if (binder == nullptr) {
        GTEST_LOG_(ERROR) << "waitForService package_native failed";
        return false;
    }
    ::android::sp<::android::content::pm::IPackageManagerNative> packageMgr =
            ::android::interface_cast<::android::content::pm::IPackageManagerNative>(binder);
    if (packageMgr == nullptr) {
        GTEST_LOG_(ERROR) << "Cannot find package manager";
        return false;
    }
    bool hasFeature = false;
    auto status = packageMgr->hasSystemFeature(::android::String16(name.c_str()), 0, &hasFeature);
    if (!status.isOk()) {
        GTEST_LOG_(ERROR) << "hasSystemFeature('" << name << "') failed: " << status;
        return false;
    }
    return hasFeature;
}

// Return the numeric value associated with a feature.
std::optional<int32_t> keymint_feature_value(bool strongbox) {
    std::string name = strongbox ? FEATURE_STRONGBOX_KEYSTORE : FEATURE_HARDWARE_KEYSTORE;
    ::android::String16 name16(name.c_str());
    ::android::sp<::android::IServiceManager> sm(::android::defaultServiceManager());
    ::android::sp<::android::IBinder> binder(
            sm->waitForService(::android::String16("package_native")));
    if (binder == nullptr) {
        GTEST_LOG_(ERROR) << "waitForService package_native failed";
        return std::nullopt;
    }
    ::android::sp<::android::content::pm::IPackageManagerNative> packageMgr =
            ::android::interface_cast<::android::content::pm::IPackageManagerNative>(binder);
    if (packageMgr == nullptr) {
        GTEST_LOG_(ERROR) << "Cannot find package manager";
        return std::nullopt;
    }

    // Package manager has no mechanism to retrieve the version of a feature,
    // only to indicate whether a certain version or above is present.
    std::optional<int32_t> result = std::nullopt;
    for (auto version : kFeatureVersions) {
        bool hasFeature = false;
        auto status = packageMgr->hasSystemFeature(name16, version, &hasFeature);
        if (!status.isOk()) {
            GTEST_LOG_(ERROR) << "hasSystemFeature('" << name << "', " << version
                              << ") failed: " << status;
            return result;
        } else if (hasFeature) {
            result = version;
        } else {
            break;
        }
    }
    return result;
}

namespace {

std::string TELEPHONY_CMD_GET_IMEI = "cmd phone get-imei ";

/*
 * Run a shell command and collect the output of it. If any error, set an empty string as the
 * output.
 */
std::string exec_command(const std::string& command) {
    char buffer[128];
    std::string result = "";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        GTEST_LOG_(ERROR) << "popen failed.";
        return result;
    }

    // read till end of process:
    while (!feof(pipe)) {
        if (fgets(buffer, 128, pipe) != NULL) {
            result += buffer;
        }
    }

    pclose(pipe);
    return result;
}

}  // namespace

/*
 * Get IMEI using Telephony service shell command. If any error while executing the command
 * then empty string will be returned as output.
 */
std::string get_imei(int slot) {
    std::string cmd = TELEPHONY_CMD_GET_IMEI + std::to_string(slot);
    std::string output = exec_command(cmd);

    if (output.empty()) {
        GTEST_LOG_(ERROR) << "Command failed. Cmd: " << cmd;
        return "";
    }

    vector<std::string> out =
            ::android::base::Tokenize(::android::base::Trim(output), "Device IMEI:");

    if (out.size() != 1) {
        GTEST_LOG_(ERROR) << "Error in parsing the command output. Cmd: " << cmd;
        return "";
    }

    std::string imei = ::android::base::Trim(out[0]);
    if (imei.compare("null") == 0) {
        GTEST_LOG_(WARNING) << "Failed to get IMEI from Telephony service: value is null. Cmd: "
                            << cmd;
        return "";
    }

    return imei;
}

std::optional<std::string> get_attestation_id(const char* prop) {
    // The frameworks code (in AndroidKeyStoreKeyPairGeneratorSpi.java) populates device ID
    // values from one of 3 places, so the same logic needs to be reproduced here so the tests
    // check what's expected correctly.
    //
    // In order of preference, the properties checked are:
    //
    // 1) `ro.product.<device-id>_for_attestation`: This should only be set in special cases; in
    //     particular, AOSP builds for reference devices use a different value than the normal
    //     builds for the same device (e.g. model of "aosp_raven" instead of "raven").
    ::android::String8 prop_name =
            ::android::String8::format("ro.product.%s_for_attestation", prop);
    std::string prop_value = ::android::base::GetProperty(prop_name.c_str(), /* default= */ "");
    if (!prop_value.empty()) {
        return prop_value;
    }

    // 2) `ro.product.vendor.<device-id>`: This property refers to the vendor code, and so is
    //    retained even in a GSI environment.
    prop_name = ::android::String8::format("ro.product.vendor.%s", prop);
    prop_value = ::android::base::GetProperty(prop_name.c_str(), /* default= */ "");
    if (!prop_value.empty()) {
        return prop_value;
    }

    // 3) `ro.product.<device-id>`: Note that this property is replaced by a default value when
    //    running a GSI environment, and so will *not* match the value expected/used by the
    //    vendor code on the device.
    prop_name = ::android::String8::format("ro.product.%s", prop);
    prop_value = ::android::base::GetProperty(prop_name.c_str(), /* default= */ "");
    if (!prop_value.empty()) {
        return prop_value;
    }

    return std::nullopt;
}

}  // namespace test

}  // namespace aidl::android::hardware::security::keymint
