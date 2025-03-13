/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <libminradio/ResponseTracker.h>

#include <aidl/android/hardware/radio/modem/BnRadioModemResponse.h>
#include <aidl/android/hardware/radio/modem/IRadioModem.h>

namespace android::hardware::radio::minimal {

class RadioModemResponseTracker
    : public ResponseTracker<::aidl::android::hardware::radio::modem::IRadioModem,
                             ::aidl::android::hardware::radio::modem::IRadioModemResponse> {
  public:
    RadioModemResponseTracker(
            std::shared_ptr<::aidl::android::hardware::radio::modem::IRadioModem> req,
            const std::shared_ptr<::aidl::android::hardware::radio::modem::IRadioModemResponse>&
                    resp);

    // TODO(now): remove if not needed
    ResponseTrackerResult<::aidl::android::hardware::radio::modem::ImeiInfo> getImei();

  protected:
    ::ndk::ScopedAStatus getImeiResponse(
            const ::aidl::android::hardware::radio::RadioResponseInfo& info,
            const std::optional<::aidl::android::hardware::radio::modem::ImeiInfo>& imei) override;
};

}  // namespace android::hardware::radio::minimal
