/*
 * Copyright (c) 2019, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <format>
#include <iomanip>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include "aidl/android/hardware/security/keymint/IRemotelyProvisionedComponent.h"

#include <aidl/android/hardware/security/keymint/RpcHardwareInfo.h>
#include <android-base/macros.h>
#include <android-base/properties.h>
#include <cppbor.h>
#include <json/json.h>
#include <keymaster/km_openssl/ec_key.h>
#include <keymaster/km_openssl/ecdsa_operation.h>
#include <keymaster/km_openssl/openssl_err.h>
#include <keymaster/km_openssl/openssl_utils.h>
#include <openssl/base64.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <remote_prov/remote_prov_utils.h>

namespace aidl::android::hardware::security::keymint::remote_prov {

constexpr uint32_t kBccPayloadIssuer = 1;
constexpr uint32_t kBccPayloadSubject = 2;
constexpr int32_t kBccPayloadSubjPubKey = -4670552;
constexpr int32_t kBccPayloadKeyUsage = -4670553;
constexpr int kP256AffinePointSize = 32;
constexpr uint32_t kNumTeeDeviceInfoEntries = 14;
constexpr std::string_view kKeyMintComponentName = "keymint";

using EC_KEY_Ptr = bssl::UniquePtr<EC_KEY>;
using EVP_PKEY_Ptr = bssl::UniquePtr<EVP_PKEY>;
using EVP_PKEY_CTX_Ptr = bssl::UniquePtr<EVP_PKEY_CTX>;
using X509_Ptr = bssl::UniquePtr<X509>;
using CRYPTO_BUFFER_Ptr = bssl::UniquePtr<CRYPTO_BUFFER>;

std::string_view deviceSuffix(std::string_view name) {
    auto pos = name.rfind('/');
    if (pos == std::string::npos) {
        return name;
    }
    return name.substr(pos + 1);
}

ErrMsgOr<bytevec> ecKeyGetPrivateKey(const EC_KEY* ecKey) {
    // Extract private key.
    const BIGNUM* bignum = EC_KEY_get0_private_key(ecKey);
    if (bignum == nullptr) {
        return "Error getting bignum from private key";
    }
    // Pad with zeros in case the length is lesser than 32.
    bytevec privKey(32, 0);
    BN_bn2binpad(bignum, privKey.data(), privKey.size());
    return privKey;
}

ErrMsgOr<bytevec> ecKeyGetPublicKey(const EC_KEY* ecKey, const int nid) {
    // Extract public key.
    auto group = EC_GROUP_Ptr(EC_GROUP_new_by_curve_name(nid));
    if (group.get() == nullptr) {
        return "Error creating EC group by curve name";
    }
    const EC_POINT* point = EC_KEY_get0_public_key(ecKey);
    if (point == nullptr) return "Error getting ecpoint from public key";

    int size =
        EC_POINT_point2oct(group.get(), point, POINT_CONVERSION_UNCOMPRESSED, nullptr, 0, nullptr);
    if (size == 0) {
        return "Error generating public key encoding";
    }

    bytevec publicKey;
    publicKey.resize(size);
    EC_POINT_point2oct(group.get(), point, POINT_CONVERSION_UNCOMPRESSED, publicKey.data(),
                       publicKey.size(), nullptr);
    return publicKey;
}

ErrMsgOr<std::tuple<bytevec, bytevec>> getAffineCoordinates(const bytevec& pubKey) {
    auto group = EC_GROUP_Ptr(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
    if (group.get() == nullptr) {
        return "Error creating EC group by curve name";
    }
    auto point = EC_POINT_Ptr(EC_POINT_new(group.get()));
    if (EC_POINT_oct2point(group.get(), point.get(), pubKey.data(), pubKey.size(), nullptr) != 1) {
        return "Error decoding publicKey";
    }
    BIGNUM_Ptr x(BN_new());
    BIGNUM_Ptr y(BN_new());
    BN_CTX_Ptr ctx(BN_CTX_new());
    if (!ctx.get()) return "Failed to create BN_CTX instance";

    if (!EC_POINT_get_affine_coordinates_GFp(group.get(), point.get(), x.get(), y.get(),
                                             ctx.get())) {
        return "Failed to get affine coordinates from ECPoint";
    }
    bytevec pubX(kP256AffinePointSize);
    bytevec pubY(kP256AffinePointSize);
    if (BN_bn2binpad(x.get(), pubX.data(), kP256AffinePointSize) != kP256AffinePointSize) {
        return "Error in converting absolute value of x coordinate to big-endian";
    }
    if (BN_bn2binpad(y.get(), pubY.data(), kP256AffinePointSize) != kP256AffinePointSize) {
        return "Error in converting absolute value of y coordinate to big-endian";
    }
    return std::make_tuple(std::move(pubX), std::move(pubY));
}

ErrMsgOr<std::tuple<bytevec, bytevec>> generateEc256KeyPair() {
    auto ec_key = EC_KEY_Ptr(EC_KEY_new());
    if (ec_key.get() == nullptr) {
        return "Failed to allocate ec key";
    }

    auto group = EC_GROUP_Ptr(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
    if (group.get() == nullptr) {
        return "Error creating EC group by curve name";
    }

    if (EC_KEY_set_group(ec_key.get(), group.get()) != 1 ||
        EC_KEY_generate_key(ec_key.get()) != 1 || EC_KEY_check_key(ec_key.get()) != 1) {
        return "Error generating key";
    }

    auto privKey = ecKeyGetPrivateKey(ec_key.get());
    if (!privKey) return privKey.moveMessage();

    auto pubKey = ecKeyGetPublicKey(ec_key.get(), NID_X9_62_prime256v1);
    if (!pubKey) return pubKey.moveMessage();

    return std::make_tuple(pubKey.moveValue(), privKey.moveValue());
}

ErrMsgOr<std::tuple<bytevec, bytevec>> generateX25519KeyPair() {
    /* Generate X25519 key pair */
    bytevec pubKey(X25519_PUBLIC_VALUE_LEN);
    bytevec privKey(X25519_PRIVATE_KEY_LEN);
    X25519_keypair(pubKey.data(), privKey.data());
    return std::make_tuple(std::move(pubKey), std::move(privKey));
}

ErrMsgOr<std::tuple<bytevec, bytevec>> generateED25519KeyPair() {
    /* Generate ED25519 key pair */
    bytevec pubKey(ED25519_PUBLIC_KEY_LEN);
    bytevec privKey(ED25519_PRIVATE_KEY_LEN);
    ED25519_keypair(pubKey.data(), privKey.data());
    return std::make_tuple(std::move(pubKey), std::move(privKey));
}

ErrMsgOr<std::tuple<bytevec, bytevec>> generateKeyPair(int32_t supportedEekCurve, bool isEek) {
    switch (supportedEekCurve) {
    case RpcHardwareInfo::CURVE_25519:
        if (isEek) {
            return generateX25519KeyPair();
        }
        return generateED25519KeyPair();
    case RpcHardwareInfo::CURVE_P256:
        return generateEc256KeyPair();
    default:
        return "Unknown EEK Curve.";
    }
}

ErrMsgOr<bytevec> constructCoseKey(int32_t supportedEekCurve, const bytevec& eekId,
                                   const bytevec& pubKey) {
    CoseKeyType keyType;
    CoseKeyAlgorithm algorithm;
    CoseKeyCurve curve;
    bytevec pubX;
    bytevec pubY;
    switch (supportedEekCurve) {
    case RpcHardwareInfo::CURVE_25519:
        keyType = OCTET_KEY_PAIR;
        algorithm = (eekId.empty()) ? EDDSA : ECDH_ES_HKDF_256;
        curve = (eekId.empty()) ? ED25519 : cppcose::X25519;
        pubX = pubKey;
        break;
    case RpcHardwareInfo::CURVE_P256: {
        keyType = EC2;
        algorithm = (eekId.empty()) ? ES256 : ECDH_ES_HKDF_256;
        curve = P256;
        auto affineCoordinates = getAffineCoordinates(pubKey);
        if (!affineCoordinates) return affineCoordinates.moveMessage();
        std::tie(pubX, pubY) = affineCoordinates.moveValue();
    } break;
    default:
        return "Unknown EEK Curve.";
    }
    cppbor::Map coseKey = cppbor::Map()
                              .add(CoseKey::KEY_TYPE, keyType)
                              .add(CoseKey::ALGORITHM, algorithm)
                              .add(CoseKey::CURVE, curve)
                              .add(CoseKey::PUBKEY_X, pubX);

    if (!pubY.empty()) coseKey.add(CoseKey::PUBKEY_Y, pubY);
    if (!eekId.empty()) coseKey.add(CoseKey::KEY_ID, eekId);

    return coseKey.canonicalize().encode();
}

bytevec kTestMacKey(32 /* count */, 0 /* byte value */);

bytevec randomBytes(size_t numBytes) {
    bytevec retval(numBytes);
    RAND_bytes(retval.data(), numBytes);
    return retval;
}

ErrMsgOr<cppbor::Array> constructCoseSign1(int32_t supportedEekCurve, const bytevec& key,
                                           const bytevec& payload, const bytevec& aad) {
    if (supportedEekCurve == RpcHardwareInfo::CURVE_P256) {
        return constructECDSACoseSign1(key, {} /* protectedParams */, payload, aad);
    } else {
        return cppcose::constructCoseSign1(key, payload, aad);
    }
}

ErrMsgOr<EekChain> generateEekChain(int32_t supportedEekCurve, size_t length,
                                    const bytevec& eekId) {
    if (length < 2) {
        return "EEK chain must contain at least 2 certs.";
    }

    auto eekChain = cppbor::Array();

    bytevec prev_priv_key;
    for (size_t i = 0; i < length - 1; ++i) {
        auto keyPair = generateKeyPair(supportedEekCurve, false);
        if (!keyPair) return keyPair.moveMessage();
        auto [pub_key, priv_key] = keyPair.moveValue();

        // The first signing key is self-signed.
        if (prev_priv_key.empty()) prev_priv_key = priv_key;

        auto coseKey = constructCoseKey(supportedEekCurve, {}, pub_key);
        if (!coseKey) return coseKey.moveMessage();

        auto coseSign1 =
            constructCoseSign1(supportedEekCurve, prev_priv_key, coseKey.moveValue(), {} /* AAD */);
        if (!coseSign1) return coseSign1.moveMessage();
        eekChain.add(coseSign1.moveValue());

        prev_priv_key = priv_key;
    }
    auto keyPair = generateKeyPair(supportedEekCurve, true);
    if (!keyPair) return keyPair.moveMessage();
    auto [pub_key, priv_key] = keyPair.moveValue();

    auto coseKey = constructCoseKey(supportedEekCurve, eekId, pub_key);
    if (!coseKey) return coseKey.moveMessage();

    auto coseSign1 =
        constructCoseSign1(supportedEekCurve, prev_priv_key, coseKey.moveValue(), {} /* AAD */);
    if (!coseSign1) return coseSign1.moveMessage();
    eekChain.add(coseSign1.moveValue());

    if (supportedEekCurve == RpcHardwareInfo::CURVE_P256) {
        // convert ec public key to x and y co-ordinates.
        auto affineCoordinates = getAffineCoordinates(pub_key);
        if (!affineCoordinates) return affineCoordinates.moveMessage();
        auto [pubX, pubY] = affineCoordinates.moveValue();
        pub_key.clear();
        pub_key.insert(pub_key.begin(), pubX.begin(), pubX.end());
        pub_key.insert(pub_key.end(), pubY.begin(), pubY.end());
    }

    return EekChain{eekChain.encode(), pub_key, priv_key};
}

bytevec getProdEekChain(int32_t supportedEekCurve) {
    cppbor::Array chain;
    if (supportedEekCurve == RpcHardwareInfo::CURVE_P256) {
        chain.add(cppbor::EncodedItem(bytevec(std::begin(kCoseEncodedEcdsa256RootCert),
                                              std::end(kCoseEncodedEcdsa256RootCert))));
        chain.add(cppbor::EncodedItem(bytevec(std::begin(kCoseEncodedEcdsa256GeekCert),
                                              std::end(kCoseEncodedEcdsa256GeekCert))));
    } else {
        chain.add(cppbor::EncodedItem(
            bytevec(std::begin(kCoseEncodedRootCert), std::end(kCoseEncodedRootCert))));
        chain.add(cppbor::EncodedItem(
            bytevec(std::begin(kCoseEncodedGeekCert), std::end(kCoseEncodedGeekCert))));
    }
    return chain.encode();
}

bool maybeOverrideAllowAnyMode(bool allowAnyMode) {
    // Use ro.build.type instead of ro.debuggable because ro.debuggable=1 for VTS testing
    std::string build_type = ::android::base::GetProperty("ro.build.type", "");
    if (!build_type.empty() && build_type != "user") {
        return true;
    }
    return allowAnyMode;
}

ErrMsgOr<std::vector<BccEntryData>> validateBcc(const cppbor::Array* bcc,
                                                hwtrust::DiceChain::Kind kind, bool allowAnyMode,
                                                bool allowDegenerate,
                                                const std::string& instanceName) {
    auto encodedBcc = bcc->encode();

    allowAnyMode = maybeOverrideAllowAnyMode(allowAnyMode);

    auto chain =
            hwtrust::DiceChain::Verify(encodedBcc, kind, allowAnyMode, deviceSuffix(instanceName));
    if (!chain.ok()) {
        return chain.error().message();
    }
    if (!allowDegenerate && !chain->IsProper()) {
        return "DICE chain is degenerate";
    }

    auto keys = chain->CosePublicKeys();
    if (!keys.ok()) {
        return keys.error().message();
    }
    std::vector<BccEntryData> result;
    for (auto& key : *keys) {
        result.push_back({std::move(key)});
    }
    return result;
}

JsonOutput jsonEncodeCsrWithBuild(const std::string& instance_name, const cppbor::Array& csr,
                                  const std::string& serialno_prop) {
    const std::string kFingerprintProp = "ro.build.fingerprint";

    if (!::android::base::WaitForPropertyCreation(kFingerprintProp)) {
        return JsonOutput::Error("Unable to read build fingerprint");
    }

    bytevec csrCbor = csr.encode();
    size_t base64Length;
    int rc = EVP_EncodedLength(&base64Length, csrCbor.size());
    if (!rc) {
        return JsonOutput::Error("Error getting base64 length. Size overflow?");
    }

    std::vector<char> base64(base64Length);
    rc = EVP_EncodeBlock(reinterpret_cast<uint8_t*>(base64.data()), csrCbor.data(), csrCbor.size());
    ++rc;  // Account for NUL, which BoringSSL does not for some reason.
    if (rc != base64Length) {
        return JsonOutput::Error("Error writing base64. Expected " + std::to_string(base64Length) +
                                 " bytes to be written, but " + std::to_string(rc) +
                                 " bytes were actually written.");
    }

    Json::Value json(Json::objectValue);
    json["name"] = instance_name;
    json["build_fingerprint"] = ::android::base::GetProperty(kFingerprintProp, /*default=*/"");
    json["serialno"] = ::android::base::GetProperty(serialno_prop, /*default=*/"");
    json["csr"] = base64.data();  // Boring writes a NUL-terminated c-string

    Json::StreamWriterBuilder factory;
    factory["indentation"] = "";  // disable pretty formatting
    return JsonOutput::Ok(Json::writeString(factory, json));
}

std::string checkMapEntry(bool isFactory, const cppbor::Map& devInfo, cppbor::MajorType majorType,
                          const std::string& entryName) {
    const std::unique_ptr<cppbor::Item>& val = devInfo.get(entryName);
    if (!val) {
        return entryName + " is missing.\n";
    }
    if (val->type() != majorType) {
        return entryName + " has the wrong type.\n";
    }
    if (isFactory) {
        return "";
    }
    switch (majorType) {
        case cppbor::TSTR:
            if (val->asTstr()->value().size() <= 0) {
                return entryName + " is present but the value is empty.\n";
            }
            break;
        case cppbor::BSTR:
            if (val->asBstr()->value().size() <= 0) {
                return entryName + " is present but the value is empty.\n";
            }
            break;
        default:
            break;
    }
    return "";
}

std::string checkMapEntry(bool isFactory, const cppbor::Map& devInfo, cppbor::MajorType majorType,
                          const std::string& entryName, const cppbor::Array& allowList) {
    std::string error = checkMapEntry(isFactory, devInfo, majorType, entryName);
    if (!error.empty()) {
        return error;
    }

    if (isFactory) {
        return "";
    }

    const std::unique_ptr<cppbor::Item>& val = devInfo.get(entryName);
    for (auto i = allowList.begin(); i != allowList.end(); ++i) {
        if (**i == *val) {
            return "";
        }
    }
    return entryName + " has an invalid value.\n";
}

std::string checkMapPatchLevelEntry(bool isFactory, const cppbor::Map& devInfo,
                                    const std::string& entryName) {
    std::string error = checkMapEntry(isFactory, devInfo, cppbor::UINT, entryName);
    if (!error.empty()) {
        return error;
    }

    if (isFactory) {
        return "";
    }

    const std::unique_ptr<cppbor::Item>& val = devInfo.get(entryName);
    std::string dateString = std::to_string(val->asUint()->unsignedValue());
    if (dateString.size() == 6) {
        dateString += "01";
    }
    if (dateString.size() != 8) {
        return entryName + " should in the format YYYYMMDD or YYYYMM\n";
    }

    std::tm t;
    std::istringstream ss(dateString);
    ss >> std::get_time(&t, "%Y%m%d");
    if (!ss) {
        return entryName + " should in the format YYYYMMDD or YYYYMM\n";
    }

    return "";
}

bool isTeeDeviceInfo(const cppbor::Map& devInfo) {
    return devInfo.get("security_level") && devInfo.get("security_level")->asTstr() &&
           devInfo.get("security_level")->asTstr()->value() == "tee";
}

ErrMsgOr<std::unique_ptr<cppbor::Map>> parseAndValidateDeviceInfo(
        const std::vector<uint8_t>& deviceInfoBytes, const RpcHardwareInfo& rpcHardwareInfo,
        bool isFactory) {
    const cppbor::Array kValidVbStates = {"green", "yellow", "orange"};
    const cppbor::Array kValidBootloaderStates = {"locked", "unlocked"};
    const cppbor::Array kValidSecurityLevels = {"tee", "strongbox"};
    const cppbor::Array kValidAttIdStates = {"locked", "open"};
    const cppbor::Array kValidFused = {0, 1};
    constexpr std::array<std::string_view, kNumTeeDeviceInfoEntries> kDeviceInfoKeys = {
            "brand",
            "manufacturer",
            "product",
            "model",
            "device",
            "vb_state",
            "bootloader_state",
            "vbmeta_digest",
            "os_version",
            "system_patch_level",
            "boot_patch_level",
            "vendor_patch_level",
            "security_level",
            "fused"};

    struct AttestationIdEntry {
        const char* id;
        bool alwaysValidate;
    };
    constexpr AttestationIdEntry kAttestationIdEntrySet[] = {{"brand", false},
                                                             {"manufacturer", true},
                                                             {"product", false},
                                                             {"model", false},
                                                             {"device", false}};

    auto [parsedVerifiedDeviceInfo, ignore1, errMsg] = cppbor::parse(deviceInfoBytes);
    if (!parsedVerifiedDeviceInfo) {
        return errMsg;
    }

    std::unique_ptr<cppbor::Map> parsed(parsedVerifiedDeviceInfo.release()->asMap());
    if (!parsed) {
        return "DeviceInfo must be a CBOR map.";
    }

    if (parsed->clone()->asMap()->canonicalize().encode() != deviceInfoBytes) {
        return "DeviceInfo ordering is non-canonical.";
    }

    if (rpcHardwareInfo.versionNumber < 3) {
        const std::unique_ptr<cppbor::Item>& version = parsed->get("version");
        if (!version) {
            return "Device info is missing version";
        }
        if (!version->asUint()) {
            return "version must be an unsigned integer";
        }
        if (version->asUint()->value() != rpcHardwareInfo.versionNumber) {
            return "DeviceInfo version (" + std::to_string(version->asUint()->value()) +
                   ") does not match the remotely provisioned component version (" +
                   std::to_string(rpcHardwareInfo.versionNumber) + ").";
        }
    }
    // Bypasses the device info validation since the device info in AVF is currently
    // empty. Check b/299256925 for more information.
    //
    // TODO(b/300911665): This check is temporary and will be replaced once the markers
    // on the DICE chain become available. We need to determine if the CSR is from the
    // RKP VM using the markers on the DICE chain.
    if (rpcHardwareInfo.uniqueId == "AVF Remote Provisioning 1") {
        return std::move(parsed);
    }

    std::string error;
    std::string tmp;
    std::set<std::string_view> previousKeys;
    switch (rpcHardwareInfo.versionNumber) {
        case 3:
            if (isTeeDeviceInfo(*parsed) && parsed->size() != kNumTeeDeviceInfoEntries) {
                error += std::format(
                        "Err: Incorrect number of device info entries. Expected {} but got "
                        "{}\n",
                        kNumTeeDeviceInfoEntries, parsed->size());
            }
            // TEE IRPC instances require all entries to be present in DeviceInfo. Non-TEE instances
            // may omit `os_version`
            if (!isTeeDeviceInfo(*parsed) && (parsed->size() != kNumTeeDeviceInfoEntries &&
                                              parsed->size() != kNumTeeDeviceInfoEntries - 1)) {
                error += std::format(
                        "Err: Incorrect number of device info entries. Expected {} or {} but got "
                        "{}\n",
                        kNumTeeDeviceInfoEntries - 1, kNumTeeDeviceInfoEntries, parsed->size());
            }
            for (auto& [key, _] : *parsed) {
                const std::string& keyValue = key->asTstr()->value();
                if (!previousKeys.insert(keyValue).second) {
                    error += "Err: Duplicate device info entry: <" + keyValue + ">,\n";
                }
                if (std::find(kDeviceInfoKeys.begin(), kDeviceInfoKeys.end(), keyValue) ==
                    kDeviceInfoKeys.end()) {
                    error += "Err: Unrecognized key entry: <" + key->asTstr()->value() + ">,\n";
                }
            }
            // Checks that only apply to v3.
            error += checkMapPatchLevelEntry(isFactory, *parsed, "system_patch_level");
            error += checkMapPatchLevelEntry(isFactory, *parsed, "boot_patch_level");
            error += checkMapPatchLevelEntry(isFactory, *parsed, "vendor_patch_level");
            FALLTHROUGH_INTENDED;
        case 2:
            for (const auto& entry : kAttestationIdEntrySet) {
                tmp = checkMapEntry(isFactory && !entry.alwaysValidate, *parsed, cppbor::TSTR,
                                    entry.id);
            }
            if (!tmp.empty()) {
                error += tmp +
                         "Attestation IDs are missing or malprovisioned. If this test is being\n"
                         "run against an early proto or EVT build, this error is probably WAI\n"
                         "and indicates that Device IDs were not provisioned in the factory. If\n"
                         "this error is returned on a DVT or later build revision, then\n"
                         "something is likely wrong with the factory provisioning process.";
            }
            // TODO: Refactor the KeyMint code that validates these fields and include it here.
            error += checkMapEntry(isFactory, *parsed, cppbor::TSTR, "vb_state", kValidVbStates);
            error += checkMapEntry(isFactory, *parsed, cppbor::TSTR, "bootloader_state",
                                   kValidBootloaderStates);
            error += checkMapEntry(isFactory, *parsed, cppbor::BSTR, "vbmeta_digest");
            error += checkMapEntry(isFactory, *parsed, cppbor::UINT, "system_patch_level");
            error += checkMapEntry(isFactory, *parsed, cppbor::UINT, "boot_patch_level");
            error += checkMapEntry(isFactory, *parsed, cppbor::UINT, "vendor_patch_level");
            error += checkMapEntry(isFactory, *parsed, cppbor::UINT, "fused", kValidFused);
            error += checkMapEntry(isFactory, *parsed, cppbor::TSTR, "security_level",
                                   kValidSecurityLevels);
            if (isTeeDeviceInfo(*parsed)) {
                error += checkMapEntry(isFactory, *parsed, cppbor::TSTR, "os_version");
            }
            break;
        case 1:
            error += checkMapEntry(isFactory, *parsed, cppbor::TSTR, "security_level",
                                   kValidSecurityLevels);
            error += checkMapEntry(isFactory, *parsed, cppbor::TSTR, "att_id_state",
                                   kValidAttIdStates);
            break;
        default:
            return "Unrecognized version: " + std::to_string(rpcHardwareInfo.versionNumber);
    }

    if (!error.empty()) {
        return error;
    }

    return std::move(parsed);
}

ErrMsgOr<std::unique_ptr<cppbor::Map>> parseAndValidateFactoryDeviceInfo(
        const std::vector<uint8_t>& deviceInfoBytes, const RpcHardwareInfo& rpcHardwareInfo) {
    return parseAndValidateDeviceInfo(deviceInfoBytes, rpcHardwareInfo, /*isFactory=*/true);
}

ErrMsgOr<std::unique_ptr<cppbor::Map>> parseAndValidateProductionDeviceInfo(
        const std::vector<uint8_t>& deviceInfoBytes, const RpcHardwareInfo& rpcHardwareInfo) {
    return parseAndValidateDeviceInfo(deviceInfoBytes, rpcHardwareInfo, /*isFactory=*/false);
}

ErrMsgOr<bytevec> getSessionKey(ErrMsgOr<std::pair<bytevec, bytevec>>& senderPubkey,
                                const EekChain& eekChain, int32_t supportedEekCurve) {
    if (supportedEekCurve == RpcHardwareInfo::CURVE_25519 ||
        supportedEekCurve == RpcHardwareInfo::CURVE_NONE) {
        return x25519_HKDF_DeriveKey(eekChain.last_pubkey, eekChain.last_privkey,
                                     senderPubkey->first, false /* senderIsA */);
    } else {
        return ECDH_HKDF_DeriveKey(eekChain.last_pubkey, eekChain.last_privkey, senderPubkey->first,
                                   false /* senderIsA */);
    }
}

ErrMsgOr<std::vector<BccEntryData>> verifyProtectedData(
        const DeviceInfo& deviceInfo, const cppbor::Array& keysToSign,
        const std::vector<uint8_t>& keysToSignMac, const ProtectedData& protectedData,
        const EekChain& eekChain, const std::vector<uint8_t>& eekId,
        const RpcHardwareInfo& rpcHardwareInfo, const std::string& instanceName,
        const std::vector<uint8_t>& challenge, bool isFactory, bool allowAnyMode = false) {
    auto [parsedProtectedData, _, protDataErrMsg] = cppbor::parse(protectedData.protectedData);
    if (!parsedProtectedData) {
        return protDataErrMsg;
    }
    if (!parsedProtectedData->asArray()) {
        return "Protected data is not a CBOR array.";
    }
    if (parsedProtectedData->asArray()->size() != kCoseEncryptEntryCount) {
        return "The protected data COSE_encrypt structure must have " +
               std::to_string(kCoseEncryptEntryCount) + " entries, but it only has " +
               std::to_string(parsedProtectedData->asArray()->size());
    }

    auto senderPubkey = getSenderPubKeyFromCoseEncrypt(parsedProtectedData);
    if (!senderPubkey) {
        return senderPubkey.message();
    }
    if (senderPubkey->second != eekId) {
        return "The COSE_encrypt recipient does not match the expected EEK identifier";
    }

    auto sessionKey = getSessionKey(senderPubkey, eekChain, rpcHardwareInfo.supportedEekCurve);
    if (!sessionKey) {
        return sessionKey.message();
    }

    auto protectedDataPayload =
            decryptCoseEncrypt(*sessionKey, parsedProtectedData.get(), bytevec{} /* aad */);
    if (!protectedDataPayload) {
        return protectedDataPayload.message();
    }

    auto [parsedPayload, __, payloadErrMsg] = cppbor::parse(*protectedDataPayload);
    if (!parsedPayload) {
        return "Failed to parse payload: " + payloadErrMsg;
    }
    if (!parsedPayload->asArray()) {
        return "The protected data payload must be an Array.";
    }
    if (parsedPayload->asArray()->size() != 3U && parsedPayload->asArray()->size() != 2U) {
        return "The protected data payload must contain SignedMAC and BCC. It may optionally "
               "contain AdditionalDKSignatures. However, the parsed payload has " +
               std::to_string(parsedPayload->asArray()->size()) + " entries.";
    }

    auto& signedMac = parsedPayload->asArray()->get(0);
    auto& bcc = parsedPayload->asArray()->get(1);
    if (!signedMac->asArray()) {
        return "The SignedMAC in the protected data payload is not an Array.";
    }
    if (!bcc->asArray()) {
        return "The BCC in the protected data payload is not an Array.";
    }

    // BCC is [ pubkey, + BccEntry]
    auto bccContents = validateBcc(bcc->asArray(), hwtrust::DiceChain::Kind::kVsr13, allowAnyMode,
                                   /*allowDegenerate=*/true, instanceName);
    if (!bccContents) {
        return bccContents.message() + "\n" + prettyPrint(bcc.get());
    }

    auto deviceInfoResult =
            parseAndValidateDeviceInfo(deviceInfo.deviceInfo, rpcHardwareInfo, isFactory);
    if (!deviceInfoResult) {
        return deviceInfoResult.message();
    }
    std::unique_ptr<cppbor::Map> deviceInfoMap = deviceInfoResult.moveValue();
    auto& signingKey = bccContents->back().pubKey;
    auto macKey = verifyAndParseCoseSign1(signedMac->asArray(), signingKey,
                                          cppbor::Array()  // SignedMacAad
                                                  .add(challenge)
                                                  .add(std::move(deviceInfoMap))
                                                  .add(keysToSignMac)
                                                  .encode());
    if (!macKey) {
        return macKey.message();
    }

    auto coseMac0 = cppbor::Array()
                            .add(cppbor::Map()  // protected
                                         .add(ALGORITHM, HMAC_256)
                                         .canonicalize()
                                         .encode())
                            .add(cppbor::Map())        // unprotected
                            .add(keysToSign.encode())  // payload (keysToSign)
                            .add(keysToSignMac);       // tag

    auto macPayload = verifyAndParseCoseMac0(&coseMac0, *macKey);
    if (!macPayload) {
        return macPayload.message();
    }

    return *bccContents;
}

ErrMsgOr<std::vector<BccEntryData>> verifyFactoryProtectedData(
        const DeviceInfo& deviceInfo, const cppbor::Array& keysToSign,
        const std::vector<uint8_t>& keysToSignMac, const ProtectedData& protectedData,
        const EekChain& eekChain, const std::vector<uint8_t>& eekId,
        const RpcHardwareInfo& rpcHardwareInfo, const std::string& instanceName,
        const std::vector<uint8_t>& challenge) {
    return verifyProtectedData(deviceInfo, keysToSign, keysToSignMac, protectedData, eekChain,
                               eekId, rpcHardwareInfo, instanceName, challenge,
                               /*isFactory=*/true);
}

ErrMsgOr<std::vector<BccEntryData>> verifyProductionProtectedData(
        const DeviceInfo& deviceInfo, const cppbor::Array& keysToSign,
        const std::vector<uint8_t>& keysToSignMac, const ProtectedData& protectedData,
        const EekChain& eekChain, const std::vector<uint8_t>& eekId,
        const RpcHardwareInfo& rpcHardwareInfo, const std::string& instanceName,
        const std::vector<uint8_t>& challenge, bool allowAnyMode) {
    return verifyProtectedData(deviceInfo, keysToSign, keysToSignMac, protectedData, eekChain,
                               eekId, rpcHardwareInfo, instanceName, challenge,
                               /*isFactory=*/false, allowAnyMode);
}

ErrMsgOr<hwtrust::DiceChain::Kind> getDiceChainKind() {
    int vendor_api_level = ::android::base::GetIntProperty("ro.vendor.api_level", -1);
    if (vendor_api_level <= __ANDROID_API_T__) {
        return hwtrust::DiceChain::Kind::kVsr13;
    } else if (vendor_api_level == __ANDROID_API_U__) {
        return hwtrust::DiceChain::Kind::kVsr14;
    } else if (vendor_api_level == 202404) {
        return hwtrust::DiceChain::Kind::kVsr15;
    } else if (vendor_api_level > 202404) {
        return hwtrust::DiceChain::Kind::kVsr16;
    } else {
        return "Unsupported vendor API level: " + std::to_string(vendor_api_level);
    }
}

ErrMsgOr<std::unique_ptr<cppbor::Array>> verifyCsr(
        const cppbor::Array& keysToSign, const std::vector<uint8_t>& encodedCsr,
        const RpcHardwareInfo& rpcHardwareInfo, const std::string& instanceName,
        const std::vector<uint8_t>& challenge, bool isFactory, bool allowAnyMode = false,
        bool allowDegenerate = true, bool requireUdsCerts = false) {
    if (rpcHardwareInfo.versionNumber != 3) {
        return "Remotely provisioned component version (" +
               std::to_string(rpcHardwareInfo.versionNumber) +
               ") does not match expected version (3).";
    }

    auto diceChainKind = getDiceChainKind();
    if (!diceChainKind) {
        return diceChainKind.message();
    }

    allowAnyMode = maybeOverrideAllowAnyMode(allowAnyMode);

    auto csr = hwtrust::Csr::validate(encodedCsr, *diceChainKind, isFactory, allowAnyMode,
                                      deviceSuffix(instanceName));

    if (!csr.ok()) {
        return csr.error().message();
    }

    if (!allowDegenerate) {
        auto diceChain = csr->getDiceChain();
        if (!diceChain.ok()) {
            return diceChain.error().message();
        }

        if (!diceChain->IsProper()) {
            return kErrorDiceChainIsDegenerate;
        }
    }

    if (requireUdsCerts && !csr->hasUdsCerts()) {
        return kErrorUdsCertsAreRequired;
    }

    auto equalChallenges = csr->compareChallenge(challenge);
    if (!equalChallenges.ok()) {
        return equalChallenges.error().message();
    }

    if (!*equalChallenges) {
        return kErrorChallengeMismatch;
    }

    auto equalKeysToSign = csr->compareKeysToSign(keysToSign.encode());
    if (!equalKeysToSign.ok()) {
        return equalKeysToSign.error().message();
    }

    if (!*equalKeysToSign) {
        return kErrorKeysToSignMismatch;
    }

    auto csrPayload = csr->getCsrPayload();
    if (!csrPayload) {
        return csrPayload.error().message();
    }

    auto [csrPayloadDecoded, _, errMsg] = cppbor::parse(*csrPayload);
    if (!csrPayloadDecoded) {
        return errMsg;
    }

    if (!csrPayloadDecoded->asArray()) {
        return "CSR payload is not an array.";
    }

    return std::unique_ptr<cppbor::Array>(csrPayloadDecoded.release()->asArray());
}

ErrMsgOr<std::unique_ptr<cppbor::Array>> verifyFactoryCsr(
        const cppbor::Array& keysToSign, const std::vector<uint8_t>& csr,
        const RpcHardwareInfo& rpcHardwareInfo, const std::string& instanceName,
        const std::vector<uint8_t>& challenge, bool allowDegenerate, bool requireUdsCerts) {
    return verifyCsr(keysToSign, csr, rpcHardwareInfo, instanceName, challenge, /*isFactory=*/true,
                     /*allowAnyMode=*/false, allowDegenerate, requireUdsCerts);
}

ErrMsgOr<std::unique_ptr<cppbor::Array>> verifyProductionCsr(const cppbor::Array& keysToSign,
                                                             const std::vector<uint8_t>& csr,
                                                             const RpcHardwareInfo& rpcHardwareInfo,
                                                             const std::string& instanceName,
                                                             const std::vector<uint8_t>& challenge,
                                                             bool allowAnyMode) {
    return verifyCsr(keysToSign, csr, rpcHardwareInfo, instanceName, challenge, /*isFactory=*/false,
                     allowAnyMode);
}

ErrMsgOr<hwtrust::DiceChain> getDiceChain(const std::vector<uint8_t>& encodedCsr, bool isFactory,
                                          bool allowAnyMode, std::string_view instanceName) {
    auto diceChainKind = getDiceChainKind();
    if (!diceChainKind) {
        return diceChainKind.message();
    }

    auto csr = hwtrust::Csr::validate(encodedCsr, *diceChainKind, isFactory, allowAnyMode,
                                      deviceSuffix(instanceName));
    if (!csr.ok()) {
        return csr.error().message();
    }

    auto diceChain = csr->getDiceChain();
    if (!diceChain.ok()) {
        return diceChain.error().message();
    }

    return std::move(*diceChain);
}

ErrMsgOr<bool> isCsrWithProperDiceChain(const std::vector<uint8_t>& encodedCsr,
                                        const std::string& instanceName) {
    auto diceChain =
            getDiceChain(encodedCsr, /*isFactory=*/false, /*allowAnyMode=*/true, instanceName);
    if (!diceChain) {
        return diceChain.message();
    }
    return diceChain->IsProper();
}

std::string hexlify(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    for (const auto& byte : bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }

    return ss.str();
}

ErrMsgOr<bool> compareRootPublicKeysInDiceChains(const std::vector<uint8_t>& encodedCsr1,
                                                 std::string_view instanceName1,
                                                 const std::vector<uint8_t>& encodedCsr2,
                                                 std::string_view instanceName2) {
    auto diceChain1 =
            getDiceChain(encodedCsr1, /*isFactory=*/false, /*allowAnyMode=*/true, instanceName1);
    if (!diceChain1) {
        return diceChain1.message();
    }

    auto proper1 = diceChain1->IsProper();
    if (!proper1) {
        return std::string(instanceName1) + " has a degenerate DICE chain:\n" +
               hexlify(encodedCsr1);
    }

    auto diceChain2 =
            getDiceChain(encodedCsr2, /*isFactory=*/false, /*allowAnyMode=*/true, instanceName2);
    if (!diceChain2) {
        return diceChain2.message();
    }

    auto proper2 = diceChain2->IsProper();
    if (!proper2) {
        return std::string(instanceName2) + " has a degenerate DICE chain:\n" +
               hexlify(encodedCsr2);
    }

    auto result = diceChain1->compareRootPublicKey(*diceChain2);
    if (!result.ok()) {
        return result.error().message();
    }

    return *result;
}

ErrMsgOr<bool> verifyComponentNameInKeyMintDiceChain(const std::vector<uint8_t>& encodedCsr) {
    auto diceChain = getDiceChain(encodedCsr, /*isFactory=*/false, /*allowAnyMode=*/true,
                                  DEFAULT_INSTANCE_NAME);
    if (!diceChain) {
        return diceChain.message();
    }

    auto satisfied = diceChain->componentNameContains(kKeyMintComponentName);
    if (!satisfied.ok()) {
        return satisfied.error().message();
    }

    return *satisfied;
}

ErrMsgOr<bool> hasNonNormalModeInDiceChain(const std::vector<uint8_t>& encodedCsr,
                                           std::string_view instanceName) {
    auto diceChain =
            getDiceChain(encodedCsr, /*isFactory=*/false, /*allowAnyMode=*/true, instanceName);
    if (!diceChain) {
        return diceChain.message();
    }

    auto hasNonNormalModeInDiceChain = diceChain->hasNonNormalMode();
    if (!hasNonNormalModeInDiceChain.ok()) {
        return hasNonNormalModeInDiceChain.error().message();
    }

    return *hasNonNormalModeInDiceChain;
}

}  // namespace aidl::android::hardware::security::keymint::remote_prov
