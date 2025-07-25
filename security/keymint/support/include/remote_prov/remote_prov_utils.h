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

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "aidl/android/hardware/security/keymint/IRemotelyProvisionedComponent.h"

#include <hwtrust/hwtrust.h>
#include <keymaster/cppcose/cppcose.h>

namespace aidl::android::hardware::security::keymint::remote_prov {

using bytevec = std::vector<uint8_t>;
using namespace cppcose;

constexpr std::string_view kErrorChallengeMismatch = "challenges do not match";
constexpr std::string_view kErrorUdsCertsAreRequired = "UdsCerts are required";
constexpr std::string_view kErrorKeysToSignMismatch = "KeysToSign do not match";
constexpr std::string_view kErrorDiceChainIsDegenerate = "DICE chain is degenerate";

extern bytevec kTestMacKey;

// The Google root key for the Endpoint Encryption Key chain, encoded as COSE_Sign1
inline constexpr uint8_t kCoseEncodedRootCert[] = {
        0x84, 0x43, 0xa1, 0x01, 0x27, 0xa0, 0x58, 0x2a, 0xa4, 0x01, 0x01, 0x03, 0x27, 0x20, 0x06,
        0x21, 0x58, 0x20, 0x99, 0xb9, 0xee, 0xdd, 0x5e, 0xe4, 0x52, 0xf6, 0x85, 0xc6, 0x4c, 0x62,
        0xdc, 0x3e, 0x61, 0xab, 0x57, 0x48, 0x7d, 0x75, 0x37, 0x29, 0xad, 0x76, 0x80, 0x32, 0xd2,
        0xb3, 0xcb, 0x63, 0x58, 0xd9, 0x58, 0x40, 0x1e, 0x22, 0x08, 0x4b, 0xa4, 0xb7, 0xa4, 0xc8,
        0xd7, 0x4e, 0x03, 0x0e, 0xfe, 0xb8, 0xaf, 0x14, 0x4c, 0xa7, 0x3b, 0x6f, 0xa5, 0xcd, 0xdc,
        0xda, 0x79, 0xc6, 0x2b, 0x64, 0xfe, 0x99, 0x39, 0xaf, 0x76, 0xe7, 0x80, 0xfa, 0x66, 0x00,
        0x85, 0x0d, 0x07, 0x98, 0x2a, 0xac, 0x91, 0x5c, 0xa7, 0x25, 0x14, 0x49, 0x06, 0x34, 0x75,
        0xca, 0x8a, 0x27, 0x7a, 0xd9, 0xe3, 0x5a, 0x49, 0xeb, 0x02, 0x03};

// The Google Endpoint Encryption Key certificate, encoded as COSE_Sign1
inline constexpr uint8_t kCoseEncodedGeekCert[] = {
        0x84, 0x43, 0xa1, 0x01, 0x27, 0xa0, 0x58, 0x4e, 0xa5, 0x01, 0x01, 0x02, 0x58, 0x20,
        0xd0, 0xae, 0xc1, 0x15, 0xca, 0x2a, 0xcf, 0x73, 0xae, 0x6b, 0xcc, 0xcb, 0xd1, 0x96,
        0x1d, 0x65, 0xe8, 0xb1, 0xdd, 0xd7, 0x4a, 0x1a, 0x37, 0xb9, 0x43, 0x3a, 0x97, 0xd5,
        0x99, 0xdf, 0x98, 0x08, 0x03, 0x38, 0x18, 0x20, 0x04, 0x21, 0x58, 0x20, 0xbe, 0x85,
        0xe7, 0x46, 0xc4, 0xa3, 0x42, 0x5a, 0x40, 0xd9, 0x36, 0x3a, 0xa6, 0x15, 0xd0, 0x2c,
        0x58, 0x7e, 0x3d, 0xdc, 0x33, 0x02, 0x32, 0xd2, 0xfc, 0x5e, 0x1e, 0x87, 0x25, 0x5f,
        0x72, 0x60, 0x58, 0x40, 0x9b, 0xcf, 0x90, 0xe2, 0x2e, 0x4b, 0xab, 0xd1, 0x18, 0xb1,
        0x0e, 0x8e, 0x5d, 0x20, 0x27, 0x4b, 0x84, 0x58, 0xfe, 0xfc, 0x32, 0x90, 0x7e, 0x72,
        0x05, 0x83, 0xbc, 0xd7, 0x82, 0xbe, 0xfa, 0x64, 0x78, 0x2d, 0x54, 0x10, 0x4b, 0xc0,
        0x31, 0xbf, 0x6b, 0xe8, 0x1e, 0x35, 0xe2, 0xf0, 0x2d, 0xce, 0x6c, 0x2f, 0x4f, 0xf2,
        0xf5, 0x4f, 0xa5, 0xd4, 0x83, 0xad, 0x96, 0xa2, 0xf1, 0x87, 0x58, 0x04};

// The Google ECDSA P256 root key for the Endpoint Encryption Key chain, encoded as COSE_Sign1
inline constexpr uint8_t kCoseEncodedEcdsa256RootCert[] = {
    0x84, 0x43, 0xa1, 0x01, 0x26, 0xa0, 0x58, 0x4d, 0xa5, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21,
    0x58, 0x20, 0xf7, 0x14, 0x8a, 0xdb, 0x97, 0xf4, 0xcc, 0x53, 0xef, 0xd2, 0x64, 0x11, 0xc4, 0xe3,
    0x75, 0x1f, 0x66, 0x1f, 0xa4, 0x71, 0x0c, 0x6c, 0xcf, 0xfa, 0x09, 0x46, 0x80, 0x74, 0x87, 0x54,
    0xf2, 0xad, 0x22, 0x58, 0x20, 0x5e, 0x7f, 0x5b, 0xf6, 0xec, 0xe4, 0xf6, 0x19, 0xcc, 0xff, 0x13,
    0x37, 0xfd, 0x0f, 0xa1, 0xc8, 0x93, 0xdb, 0x18, 0x06, 0x76, 0xc4, 0x5d, 0xe6, 0xd7, 0x6a, 0x77,
    0x86, 0xc3, 0x2d, 0xaf, 0x8f, 0x58, 0x40, 0x2f, 0x97, 0x8e, 0x42, 0xfb, 0xbe, 0x07, 0x2d, 0x95,
    0x47, 0x85, 0x47, 0x93, 0x40, 0xb0, 0x1f, 0xd4, 0x9b, 0x47, 0xa4, 0xc4, 0x44, 0xa9, 0xf2, 0xa1,
    0x07, 0x87, 0x10, 0xc7, 0x9f, 0xcb, 0x11, 0xf4, 0xbf, 0x9f, 0xe8, 0x3b, 0xe0, 0xe7, 0x34, 0x4c,
    0x15, 0xfc, 0x7b, 0xc3, 0x7e, 0x33, 0x05, 0xf4, 0xd1, 0x34, 0x3c, 0xed, 0x02, 0x04, 0x60, 0x7a,
    0x15, 0xe0, 0x79, 0xd3, 0x8a, 0xff, 0x24};

// The Google ECDSA P256 Endpoint Encryption Key certificate, encoded as COSE_Sign1
inline constexpr uint8_t kCoseEncodedEcdsa256GeekCert[] = {
    0x84, 0x43, 0xa1, 0x01, 0x26, 0xa0, 0x58, 0x71, 0xa6, 0x01, 0x02, 0x02, 0x58, 0x20, 0x35, 0x73,
    0xb7, 0x3f, 0xa0, 0x8a, 0x80, 0x89, 0xb1, 0x26, 0x67, 0xe9, 0xcb, 0x7c, 0x75, 0xa1, 0xaf, 0x02,
    0x61, 0xfc, 0x6e, 0x65, 0x03, 0x91, 0x3b, 0xd3, 0x4b, 0x7d, 0x14, 0x94, 0x3e, 0x46, 0x03, 0x38,
    0x18, 0x20, 0x01, 0x21, 0x58, 0x20, 0xe0, 0x41, 0xcf, 0x2f, 0x0f, 0x34, 0x0f, 0x1c, 0x33, 0x2c,
    0x41, 0xb0, 0xcf, 0xd7, 0x0c, 0x30, 0x55, 0x35, 0xd2, 0x1e, 0x6a, 0x47, 0x13, 0x4b, 0x2e, 0xd1,
    0x48, 0x96, 0x7e, 0x24, 0x9c, 0x68, 0x22, 0x58, 0x20, 0x1f, 0xce, 0x45, 0xc5, 0xfb, 0x61, 0xba,
    0x81, 0x21, 0xf9, 0xe5, 0x05, 0x9b, 0x9b, 0x39, 0x0e, 0x76, 0x86, 0x86, 0x47, 0xb8, 0x1e, 0x2f,
    0x45, 0xf1, 0xce, 0xaf, 0xda, 0x3f, 0x80, 0x68, 0xdb, 0x58, 0x40, 0x8c, 0xb3, 0xba, 0x7e, 0x20,
    0x3e, 0x32, 0xb0, 0x68, 0xdf, 0x60, 0xd1, 0x1d, 0x7d, 0xf0, 0xac, 0x38, 0x8e, 0x51, 0xbc, 0xff,
    0x6c, 0xe1, 0x67, 0x3b, 0x4a, 0x79, 0xbc, 0x56, 0x78, 0xb3, 0x99, 0xd8, 0x7c, 0x8a, 0x07, 0xd8,
    0xda, 0xb5, 0xb5, 0x7f, 0x71, 0xf4, 0xd8, 0x6b, 0xdf, 0x33, 0x27, 0x34, 0x7b, 0x65, 0xd1, 0x2a,
    0xeb, 0x86, 0x99, 0x98, 0xab, 0x3a, 0xb4, 0x80, 0xaa, 0xbd, 0x50};

/**
 * Generates random bytes.
 */
bytevec randomBytes(size_t numBytes);

const std::string DEFAULT_INSTANCE_NAME =
        "android.hardware.security.keymint.IRemotelyProvisionedComponent/default";
const std::string RKPVM_INSTANCE_NAME =
        "android.hardware.security.keymint.IRemotelyProvisionedComponent/avf";

/**
 * Returns the portion of an instance name after the /
 * e.g. for "android.hardware.security.keymint.IRemotelyProvisionedComponent/avf",
 * it returns "avf".
 */
std::string_view deviceSuffix(std::string_view name);

struct EekChain {
    bytevec chain;
    bytevec last_pubkey;
    bytevec last_privkey;
};

/**
 * Based on the supportedEekCurve, Generates an X25519/ECDH with the specified eekId
 * and an Ed25519/ECDSA chain of the specified length. All keys are generated randomly.
 */
ErrMsgOr<EekChain> generateEekChain(int32_t supportedEekCurve, size_t length, const bytevec& eekId);

/**
 * Returns the CBOR-encoded, production Google Endpoint Encryption Key chain.
 */
bytevec getProdEekChain(int32_t supportedEekCurve);

struct BccEntryData {
    bytevec pubKey;
};

struct JsonOutput {
    static JsonOutput Ok(std::string json) { return {std::move(json), ""}; }
    static JsonOutput Error(std::string error) { return {"", std::move(error)}; }

    std::string output;
    std::string error;  // if non-empty, this describes what went wrong
};

/**
 * Take a given instance name and certificate request, then output a JSON blob
 * containing the name, build fingerprint and certificate request. This data may
 * be serialized, then later uploaded to the remote provisioning service. The
 * input csr is not validated, only encoded.
 *
 * Output format:
 *   {
 *     "build_fingerprint": <string>
 *     "csr": <base64 CBOR CSR>
 *     "name": <string>
 *   }
 */
JsonOutput jsonEncodeCsrWithBuild(const std::string& instance_name, const cppbor::Array& csr,
                                  const std::string& serialno_prop);

/**
 * Parses a DeviceInfo structure from the given CBOR data. The parsed data is then validated to
 * ensure it contains the minimum required data at the time of manufacturing. This is only a
 * partial validation, as some fields may not be provisioned yet at the time this information
 * is parsed in the manufacturing process.
 */
ErrMsgOr<std::unique_ptr<cppbor::Map>> parseAndValidateFactoryDeviceInfo(
        const std::vector<uint8_t>& deviceInfoBytes, const RpcHardwareInfo& info);

/**
 * Parses a DeviceInfo structure from the given CBOR data. The parsed data is then validated to
 * ensure it is formatted correctly and that it contains the required values for Remote Key
 * Provisioning. This is a full validation, and assumes the device is provisioned as if it is
 * suitable for the end user.
 */
ErrMsgOr<std::unique_ptr<cppbor::Map>> parseAndValidateProductionDeviceInfo(
        const std::vector<uint8_t>& deviceInfoBytes, const RpcHardwareInfo& info);

/**
 * Verify the protected data as if the device is still early in the factory process and may not
 * have all device identifiers provisioned yet.
 */
ErrMsgOr<std::vector<BccEntryData>> verifyFactoryProtectedData(
        const DeviceInfo& deviceInfo, const cppbor::Array& keysToSign,
        const std::vector<uint8_t>& keysToSignMac, const ProtectedData& protectedData,
        const EekChain& eekChain, const std::vector<uint8_t>& eekId, const RpcHardwareInfo& info,
        const std::string& instanceName, const std::vector<uint8_t>& challenge);
/**
 * Verify the protected data as if the device is a final production sample.
 */
ErrMsgOr<std::vector<BccEntryData>> verifyProductionProtectedData(
        const DeviceInfo& deviceInfo, const cppbor::Array& keysToSign,
        const std::vector<uint8_t>& keysToSignMac, const ProtectedData& protectedData,
        const EekChain& eekChain, const std::vector<uint8_t>& eekId, const RpcHardwareInfo& info,
        const std::string& instanceName, const std::vector<uint8_t>& challenge,
        bool allowAnyMode = false);

/**
 * Verify the CSR as if the device is still early in the factory process and may not
 * have all device identifiers provisioned yet.
 */
ErrMsgOr<std::unique_ptr<cppbor::Array>> verifyFactoryCsr(const cppbor::Array& keysToSign,
                                                          const std::vector<uint8_t>& csr,
                                                          const RpcHardwareInfo& info,
                                                          const std::string& instanceName,
                                                          const std::vector<uint8_t>& challenge,
                                                          bool allowDegenerate = true,
                                                          bool requireUdsCerts = false);

/**
 * Verify the CSR as if the device is a final production sample.
 */
ErrMsgOr<std::unique_ptr<cppbor::Array>> verifyProductionCsr(const cppbor::Array& keysToSign,
                                                             const std::vector<uint8_t>& csr,
                                                             const RpcHardwareInfo& info,
                                                             const std::string& instanceName,
                                                             const std::vector<uint8_t>& challenge,
                                                             bool allowAnyMode = false);

/** Checks whether the CSR has a proper DICE chain. */
ErrMsgOr<bool> isCsrWithProperDiceChain(const std::vector<uint8_t>& csr,
                                        const std::string& instanceName);

/** Checks whether the CSRs contain DICE certificate chains that have root certificates
 * with the same public key.
 */
ErrMsgOr<bool> compareRootPublicKeysInDiceChains(const std::vector<uint8_t>& csr1,
                                                 std::string_view instanceName1,
                                                 const std::vector<uint8_t>& csr2,
                                                 std::string_view instanceName2);

/** Checks whether the component name in the configuration descriptor in the leaf certificate
 * of the primary KeyMint instance's DICE certificate chain contains "keymint"
 */
ErrMsgOr<bool> verifyComponentNameInKeyMintDiceChain(const std::vector<uint8_t>& csr);

/** Checks whether the DICE chain in the CSR has a certificate with a non-normal mode. */
ErrMsgOr<bool> hasNonNormalModeInDiceChain(const std::vector<uint8_t>& csr,
                                           std::string_view instanceName);

/** Verify the DICE chain. */
ErrMsgOr<std::vector<BccEntryData>> validateBcc(const cppbor::Array* bcc,
                                                hwtrust::DiceChain::Kind kind, bool allowAnyMode,
                                                bool allowDegenerate,
                                                const std::string& instanceName);

}  // namespace aidl::android::hardware::security::keymint::remote_prov
