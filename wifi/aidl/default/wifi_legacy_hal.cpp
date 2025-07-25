/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "wifi_legacy_hal.h"

#include <android-base/logging.h>
#include <cutils/properties.h>
#include <net/if.h>

#include <array>
#include <chrono>

#include "aidl_sync_util.h"
#include "wifi_legacy_hal_stubs.h"

namespace {
// Constants ported over from the legacy HAL calling code
// (com_android_server_wifi_WifiNative.cpp). This will all be thrown
// away when this shim layer is replaced by the real vendor
// implementation.
static constexpr uint32_t kMaxVersionStringLength = 256;
static constexpr uint32_t kMaxCachedGscanResults = 64;
static constexpr uint32_t kMaxGscanFrequenciesForBand = 64;
static constexpr uint32_t kLinkLayerStatsDataMpduSizeThreshold = 128;
static constexpr uint32_t kMaxWakeReasonStatsArraySize = 32;
static constexpr uint32_t kMaxRingBuffers = 10;
static constexpr uint32_t kMaxWifiUsableChannels = 256;
static constexpr uint32_t kMaxSupportedRadioCombinationsMatrixLength = 256;
// Need a long timeout (1000ms) for chips that unload their driver.
static constexpr uint32_t kMaxStopCompleteWaitMs = 1000;
static constexpr char kDriverPropName[] = "wlan.driver.status";

// Helper function to create a non-const char* for legacy Hal API's.
std::vector<char> makeCharVec(const std::string& str) {
    std::vector<char> vec(str.size() + 1);
    vec.assign(str.begin(), str.end());
    vec.push_back('\0');
    return vec;
}
}  // namespace

namespace aidl {
namespace android {
namespace hardware {
namespace wifi {
namespace legacy_hal {

// Legacy HAL functions accept "C" style function pointers, so use global
// functions to pass to the legacy HAL function and store the corresponding
// std::function methods to be invoked.
//
// Callback to be invoked once |stop| is complete
std::function<void(wifi_handle handle)> on_stop_complete_internal_callback;
void onAsyncStopComplete(wifi_handle handle) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_stop_complete_internal_callback) {
        on_stop_complete_internal_callback(handle);
        // Invalidate this callback since we don't want this firing again.
        on_stop_complete_internal_callback = nullptr;
    }
}

// Callback to be invoked for driver dump.
std::function<void(char*, int)> on_driver_memory_dump_internal_callback;
void onSyncDriverMemoryDump(char* buffer, int buffer_size) {
    if (on_driver_memory_dump_internal_callback) {
        on_driver_memory_dump_internal_callback(buffer, buffer_size);
    }
}

// Callback to be invoked for firmware dump.
std::function<void(char*, int)> on_firmware_memory_dump_internal_callback;
void onSyncFirmwareMemoryDump(char* buffer, int buffer_size) {
    if (on_firmware_memory_dump_internal_callback) {
        on_firmware_memory_dump_internal_callback(buffer, buffer_size);
    }
}

// Callback to be invoked for Gscan events.
std::function<void(wifi_request_id, wifi_scan_event)> on_gscan_event_internal_callback;
void onAsyncGscanEvent(wifi_request_id id, wifi_scan_event event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_gscan_event_internal_callback) {
        on_gscan_event_internal_callback(id, event);
    }
}

// Callback to be invoked for Gscan full results.
std::function<void(wifi_request_id, wifi_scan_result*, uint32_t)>
        on_gscan_full_result_internal_callback;
void onAsyncGscanFullResult(wifi_request_id id, wifi_scan_result* result,
                            uint32_t buckets_scanned) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_gscan_full_result_internal_callback) {
        on_gscan_full_result_internal_callback(id, result, buckets_scanned);
    }
}

// Callback to be invoked for link layer stats results.
std::function<void((wifi_request_id, wifi_iface_stat*, int, wifi_radio_stat*))>
        on_link_layer_stats_result_internal_callback;
void onSyncLinkLayerStatsResult(wifi_request_id id, wifi_iface_stat* iface_stat, int num_radios,
                                wifi_radio_stat* radio_stat) {
    if (on_link_layer_stats_result_internal_callback) {
        on_link_layer_stats_result_internal_callback(id, iface_stat, num_radios, radio_stat);
    }
}

// Callback to be invoked for Multi link layer stats results.
std::function<void((wifi_request_id, wifi_iface_ml_stat*, int, wifi_radio_stat*))>
        on_link_layer_ml_stats_result_internal_callback;
void onSyncLinkLayerMlStatsResult(wifi_request_id id, wifi_iface_ml_stat* iface_ml_stat,
                                  int num_radios, wifi_radio_stat* radio_stat) {
    if (on_link_layer_ml_stats_result_internal_callback) {
        on_link_layer_ml_stats_result_internal_callback(id, iface_ml_stat, num_radios, radio_stat);
    }
}

// Callback to be invoked for rssi threshold breach.
std::function<void((wifi_request_id, uint8_t*, int8_t))>
        on_rssi_threshold_breached_internal_callback;
void onAsyncRssiThresholdBreached(wifi_request_id id, uint8_t* bssid, int8_t rssi) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_rssi_threshold_breached_internal_callback) {
        on_rssi_threshold_breached_internal_callback(id, bssid, rssi);
    }
}

// Callback to be invoked for ring buffer data indication.
std::function<void(char*, char*, int, wifi_ring_buffer_status*)>
        on_ring_buffer_data_internal_callback;
void onAsyncRingBufferData(char* ring_name, char* buffer, int buffer_size,
                           wifi_ring_buffer_status* status) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_ring_buffer_data_internal_callback) {
        on_ring_buffer_data_internal_callback(ring_name, buffer, buffer_size, status);
    }
}

// Callback to be invoked for error alert indication.
std::function<void(wifi_request_id, char*, int, int)> on_error_alert_internal_callback;
void onAsyncErrorAlert(wifi_request_id id, char* buffer, int buffer_size, int err_code) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_error_alert_internal_callback) {
        on_error_alert_internal_callback(id, buffer, buffer_size, err_code);
    }
}

// Callback to be invoked for radio mode change indication.
std::function<void(wifi_request_id, uint32_t, wifi_mac_info*)>
        on_radio_mode_change_internal_callback;
void onAsyncRadioModeChange(wifi_request_id id, uint32_t num_macs, wifi_mac_info* mac_infos) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_radio_mode_change_internal_callback) {
        on_radio_mode_change_internal_callback(id, num_macs, mac_infos);
    }
}

// Callback to be invoked to report subsystem restart
std::function<void(const char*)> on_subsystem_restart_internal_callback;
void onAsyncSubsystemRestart(const char* error) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_subsystem_restart_internal_callback) {
        on_subsystem_restart_internal_callback(error);
    }
}

// Callback to be invoked for rtt results results.
std::function<void(wifi_request_id, unsigned num_results, wifi_rtt_result* rtt_results[])>
        on_rtt_results_internal_callback;
std::function<void(wifi_request_id, unsigned num_results, wifi_rtt_result_v2* rtt_results_v2[])>
        on_rtt_results_internal_callback_v2;
std::function<void(wifi_request_id, unsigned num_results, wifi_rtt_result_v3* rtt_results_v3[])>
        on_rtt_results_internal_callback_v3;
std::function<void(wifi_request_id, unsigned num_results, wifi_rtt_result_v4* rtt_results_v4[])>
        on_rtt_results_internal_callback_v4;

void invalidateRttResultsCallbacks() {
    on_rtt_results_internal_callback = nullptr;
    on_rtt_results_internal_callback_v2 = nullptr;
    on_rtt_results_internal_callback_v3 = nullptr;
    on_rtt_results_internal_callback_v4 = nullptr;
};

void onAsyncRttResults(wifi_request_id id, unsigned num_results, wifi_rtt_result* rtt_results[]) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_rtt_results_internal_callback) {
        on_rtt_results_internal_callback(id, num_results, rtt_results);
        invalidateRttResultsCallbacks();
    }
}

void onAsyncRttResultsV2(wifi_request_id id, unsigned num_results,
                         wifi_rtt_result_v2* rtt_results_v2[]) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_rtt_results_internal_callback_v2) {
        on_rtt_results_internal_callback_v2(id, num_results, rtt_results_v2);
        invalidateRttResultsCallbacks();
    }
}

void onAsyncRttResultsV3(wifi_request_id id, unsigned num_results,
                         wifi_rtt_result_v3* rtt_results_v3[]) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_rtt_results_internal_callback_v3) {
        on_rtt_results_internal_callback_v3(id, num_results, rtt_results_v3);
        invalidateRttResultsCallbacks();
    }
}

void onAsyncRttResultsV4(wifi_request_id id, unsigned num_results,
                         wifi_rtt_result_v4* rtt_results_v4[]) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_rtt_results_internal_callback_v4) {
        on_rtt_results_internal_callback_v4(id, num_results, rtt_results_v4);
        invalidateRttResultsCallbacks();
    }
}

// Callbacks for the various NAN operations.
// NOTE: These have very little conversions to perform before invoking the user
// callbacks.
// So, handle all of them here directly to avoid adding an unnecessary layer.
std::function<void(transaction_id, const NanResponseMsg&)> on_nan_notify_response_user_callback;
void onAsyncNanNotifyResponse(transaction_id id, NanResponseMsg* msg) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_notify_response_user_callback && msg) {
        on_nan_notify_response_user_callback(id, *msg);
    }
}

std::function<void(const NanPublishRepliedInd&)> on_nan_event_publish_replied_user_callback;
void onAsyncNanEventPublishReplied(NanPublishRepliedInd* /* event */) {
    LOG(ERROR) << "onAsyncNanEventPublishReplied triggered";
}

std::function<void(const NanPublishTerminatedInd&)> on_nan_event_publish_terminated_user_callback;
void onAsyncNanEventPublishTerminated(NanPublishTerminatedInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_publish_terminated_user_callback && event) {
        on_nan_event_publish_terminated_user_callback(*event);
    }
}

std::function<void(const NanMatchInd&)> on_nan_event_match_user_callback;
void onAsyncNanEventMatch(NanMatchInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_match_user_callback && event) {
        on_nan_event_match_user_callback(*event);
    }
}

std::function<void(const NanMatchExpiredInd&)> on_nan_event_match_expired_user_callback;
void onAsyncNanEventMatchExpired(NanMatchExpiredInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_match_expired_user_callback && event) {
        on_nan_event_match_expired_user_callback(*event);
    }
}

std::function<void(const NanSubscribeTerminatedInd&)>
        on_nan_event_subscribe_terminated_user_callback;
void onAsyncNanEventSubscribeTerminated(NanSubscribeTerminatedInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_subscribe_terminated_user_callback && event) {
        on_nan_event_subscribe_terminated_user_callback(*event);
    }
}

std::function<void(const NanFollowupInd&)> on_nan_event_followup_user_callback;
void onAsyncNanEventFollowup(NanFollowupInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_followup_user_callback && event) {
        on_nan_event_followup_user_callback(*event);
    }
}

std::function<void(const NanDiscEngEventInd&)> on_nan_event_disc_eng_event_user_callback;
void onAsyncNanEventDiscEngEvent(NanDiscEngEventInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_disc_eng_event_user_callback && event) {
        on_nan_event_disc_eng_event_user_callback(*event);
    }
}

std::function<void(const NanDisabledInd&)> on_nan_event_disabled_user_callback;
void onAsyncNanEventDisabled(NanDisabledInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_disabled_user_callback && event) {
        on_nan_event_disabled_user_callback(*event);
    }
}

std::function<void(const NanTCAInd&)> on_nan_event_tca_user_callback;
void onAsyncNanEventTca(NanTCAInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_tca_user_callback && event) {
        on_nan_event_tca_user_callback(*event);
    }
}

std::function<void(const NanBeaconSdfPayloadInd&)> on_nan_event_beacon_sdf_payload_user_callback;
void onAsyncNanEventBeaconSdfPayload(NanBeaconSdfPayloadInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_beacon_sdf_payload_user_callback && event) {
        on_nan_event_beacon_sdf_payload_user_callback(*event);
    }
}

std::function<void(const NanDataPathRequestInd&)> on_nan_event_data_path_request_user_callback;
void onAsyncNanEventDataPathRequest(NanDataPathRequestInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_data_path_request_user_callback && event) {
        on_nan_event_data_path_request_user_callback(*event);
    }
}
std::function<void(const NanDataPathConfirmInd&)> on_nan_event_data_path_confirm_user_callback;
void onAsyncNanEventDataPathConfirm(NanDataPathConfirmInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_data_path_confirm_user_callback && event) {
        on_nan_event_data_path_confirm_user_callback(*event);
    }
}

std::function<void(const NanDataPathEndInd&)> on_nan_event_data_path_end_user_callback;
void onAsyncNanEventDataPathEnd(NanDataPathEndInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_data_path_end_user_callback && event) {
        on_nan_event_data_path_end_user_callback(*event);
    }
}

std::function<void(const NanTransmitFollowupInd&)> on_nan_event_transmit_follow_up_user_callback;
void onAsyncNanEventTransmitFollowUp(NanTransmitFollowupInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_transmit_follow_up_user_callback && event) {
        on_nan_event_transmit_follow_up_user_callback(*event);
    }
}

std::function<void(const NanRangeRequestInd&)> on_nan_event_range_request_user_callback;
void onAsyncNanEventRangeRequest(NanRangeRequestInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_range_request_user_callback && event) {
        on_nan_event_range_request_user_callback(*event);
    }
}

std::function<void(const NanRangeReportInd&)> on_nan_event_range_report_user_callback;
void onAsyncNanEventRangeReport(NanRangeReportInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_range_report_user_callback && event) {
        on_nan_event_range_report_user_callback(*event);
    }
}

std::function<void(const NanDataPathScheduleUpdateInd&)> on_nan_event_schedule_update_user_callback;
void onAsyncNanEventScheduleUpdate(NanDataPathScheduleUpdateInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_schedule_update_user_callback && event) {
        on_nan_event_schedule_update_user_callback(*event);
    }
}

std::function<void(const NanSuspensionModeChangeInd&)>
        on_nan_event_suspension_mode_change_user_callback;
void onAsyncNanEventSuspensionModeChange(NanSuspensionModeChangeInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_suspension_mode_change_user_callback && event) {
        on_nan_event_suspension_mode_change_user_callback(*event);
    }
}

std::function<void(wifi_rtt_result* rtt_results[], uint32_t num_results, uint16_t session_id)>
        on_nan_event_ranging_results_callback;
void onAsyncNanEventRangingResults(wifi_rtt_result* rtt_results[], uint32_t num_results,
                                   uint16_t session_id) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_ranging_results_callback && rtt_results) {
        on_nan_event_ranging_results_callback(rtt_results, num_results, session_id);
    }
}

std::function<void(const NanPairingRequestInd&)> on_nan_event_pairing_request_user_callback;
void onAsyncNanEventPairingRequest(NanPairingRequestInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_pairing_request_user_callback && event) {
        on_nan_event_pairing_request_user_callback(*event);
    }
}

std::function<void(const NanPairingConfirmInd&)> on_nan_event_pairing_confirm_user_callback;
void onAsyncNanEventPairingConfirm(NanPairingConfirmInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_pairing_confirm_user_callback && event) {
        on_nan_event_pairing_confirm_user_callback(*event);
    }
}

std::function<void(const NanBootstrappingRequestInd&)>
        on_nan_event_bootstrapping_request_user_callback;
void onAsyncNanEventBootstrappingRequest(NanBootstrappingRequestInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_bootstrapping_request_user_callback && event) {
        on_nan_event_bootstrapping_request_user_callback(*event);
    }
}

std::function<void(const NanBootstrappingConfirmInd&)>
        on_nan_event_bootstrapping_confirm_user_callback;
void onAsyncNanEventBootstrappingConfirm(NanBootstrappingConfirmInd* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_nan_event_bootstrapping_confirm_user_callback && event) {
        on_nan_event_bootstrapping_confirm_user_callback(*event);
    }
}

// Callbacks for the various TWT operations.
std::function<void(const TwtSetupResponse&)> on_twt_event_setup_response_callback;
void onAsyncTwtEventSetupResponse(TwtSetupResponse* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_event_setup_response_callback && event) {
        on_twt_event_setup_response_callback(*event);
    }
}

std::function<void(const TwtTeardownCompletion&)> on_twt_event_teardown_completion_callback;
void onAsyncTwtEventTeardownCompletion(TwtTeardownCompletion* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_event_teardown_completion_callback && event) {
        on_twt_event_teardown_completion_callback(*event);
    }
}

std::function<void(const TwtInfoFrameReceived&)> on_twt_event_info_frame_received_callback;
void onAsyncTwtEventInfoFrameReceived(TwtInfoFrameReceived* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_event_info_frame_received_callback && event) {
        on_twt_event_info_frame_received_callback(*event);
    }
}

std::function<void(const TwtDeviceNotify&)> on_twt_event_device_notify_callback;
void onAsyncTwtEventDeviceNotify(TwtDeviceNotify* event) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_event_device_notify_callback && event) {
        on_twt_event_device_notify_callback(*event);
    }
}

// Callback to report current CHRE NAN state
std::function<void(chre_nan_rtt_state)> on_chre_nan_rtt_internal_callback;
void onAsyncChreNanRttState(chre_nan_rtt_state state) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_chre_nan_rtt_internal_callback) {
        on_chre_nan_rtt_internal_callback(state);
    }
}

// Callback to report cached scan results
std::function<void(wifi_cached_scan_report*)> on_cached_scan_results_internal_callback;
void onSyncCachedScanResults(wifi_cached_scan_report* cache_report) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_cached_scan_results_internal_callback) {
        on_cached_scan_results_internal_callback(cache_report);
    }
}

// Callback to be invoked for TWT failure
std::function<void((wifi_request_id, wifi_twt_error_code error_code))>
        on_twt_failure_internal_callback;
void onAsyncTwtError(wifi_request_id id, wifi_twt_error_code error_code) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_failure_internal_callback) {
        on_twt_failure_internal_callback(id, error_code);
    }
}

// Callback to be invoked for TWT session creation
std::function<void((wifi_request_id, wifi_twt_session twt_session))>
        on_twt_session_create_internal_callback;
void onAsyncTwtSessionCreate(wifi_request_id id, wifi_twt_session twt_session) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_session_create_internal_callback) {
        on_twt_session_create_internal_callback(id, twt_session);
    }
}

// Callback to be invoked for TWT session update
std::function<void((wifi_request_id, wifi_twt_session twt_session))>
        on_twt_session_update_internal_callback;
void onAsyncTwtSessionUpdate(wifi_request_id id, wifi_twt_session twt_session) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_session_update_internal_callback) {
        on_twt_session_update_internal_callback(id, twt_session);
    }
}

// Callback to be invoked for TWT session teardown
std::function<void(
        (wifi_request_id, int twt_session_id, wifi_twt_teardown_reason_code reason_code))>
        on_twt_session_teardown_internal_callback;
void onAsyncTwtSessionTeardown(wifi_request_id id, int twt_session_id,
                               wifi_twt_teardown_reason_code reason_code) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_session_teardown_internal_callback) {
        on_twt_session_teardown_internal_callback(id, twt_session_id, reason_code);
    }
}

// Callback to be invoked for TWT session get stats
std::function<void((wifi_request_id, int twt_session_id, wifi_twt_session_stats stats))>
        on_twt_session_stats_internal_callback;
void onAsyncTwtSessionStats(wifi_request_id id, int twt_session_id, wifi_twt_session_stats stats) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_session_stats_internal_callback) {
        on_twt_session_stats_internal_callback(id, twt_session_id, stats);
    }
}

// Callback to be invoked for TWT session suspend
std::function<void((wifi_request_id, int twt_session_id))> on_twt_session_suspend_internal_callback;
void onAsyncTwtSessionSuspend(wifi_request_id id, int twt_session_id) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_session_suspend_internal_callback) {
        on_twt_session_suspend_internal_callback(id, twt_session_id);
    }
}

// Callback to be invoked for TWT session resume
std::function<void((wifi_request_id, int twt_session_id))> on_twt_session_resume_internal_callback;
void onAsyncTwtSessionResume(wifi_request_id id, int twt_session_id) {
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (on_twt_session_resume_internal_callback) {
        on_twt_session_resume_internal_callback(id, twt_session_id);
    }
}

// End of the free-standing "C" style callbacks.

WifiLegacyHal::WifiLegacyHal(const std::weak_ptr<::android::wifi_system::InterfaceTool> iface_tool,
                             const wifi_hal_fn& fn, bool is_primary)
    : global_func_table_(fn),
      global_handle_(nullptr),
      awaiting_event_loop_termination_(false),
      is_started_(false),
      iface_tool_(iface_tool),
      is_primary_(is_primary) {}

wifi_error WifiLegacyHal::initialize() {
    LOG(DEBUG) << "Initialize legacy HAL";
    // this now does nothing, since HAL function table is provided
    // to the constructor
    return WIFI_SUCCESS;
}

wifi_error WifiLegacyHal::start() {
    // Ensure that we're starting in a good state.
    CHECK(global_func_table_.wifi_initialize && !global_handle_ && iface_name_to_handle_.empty() &&
          !awaiting_event_loop_termination_);
    if (is_started_) {
        LOG(DEBUG) << "Legacy HAL already started";
        return WIFI_SUCCESS;
    }
    LOG(DEBUG) << "Waiting for the driver ready";
    wifi_error status = global_func_table_.wifi_wait_for_driver_ready();
    if (status == WIFI_ERROR_TIMED_OUT || status == WIFI_ERROR_UNKNOWN) {
        LOG(ERROR) << "Failed or timed out awaiting driver ready";
        return status;
    }

    if (is_primary_) {
        property_set(kDriverPropName, "ok");

        if (!iface_tool_.lock()->SetWifiUpState(true)) {
            LOG(ERROR) << "Failed to set WiFi interface up";
            return WIFI_ERROR_UNKNOWN;
        }
    }

    LOG(DEBUG) << "Starting legacy HAL";
    status = global_func_table_.wifi_initialize(&global_handle_);
    if (status != WIFI_SUCCESS || !global_handle_) {
        LOG(ERROR) << "Failed to retrieve global handle";
        return status;
    }
    std::thread(&WifiLegacyHal::runEventLoop, this).detach();
    status = retrieveIfaceHandles();
    if (status != WIFI_SUCCESS || iface_name_to_handle_.empty()) {
        LOG(ERROR) << "Failed to retrieve wlan interface handle";
        return status;
    }
    LOG(DEBUG) << "Legacy HAL start complete";
    is_started_ = true;
    return WIFI_SUCCESS;
}

wifi_error WifiLegacyHal::stop(
        /* NONNULL */ std::unique_lock<std::recursive_mutex>* lock,
        const std::function<void()>& on_stop_complete_user_callback) {
    if (!is_started_) {
        LOG(DEBUG) << "Legacy HAL already stopped";
        on_stop_complete_user_callback();
        return WIFI_SUCCESS;
    }
    LOG(DEBUG) << "Stopping legacy HAL";
    on_stop_complete_internal_callback = [on_stop_complete_user_callback,
                                          this](wifi_handle handle) {
        CHECK_EQ(global_handle_, handle) << "Handle mismatch";
        LOG(INFO) << "Legacy HAL stop complete callback received";
        // Invalidate all the internal pointers now that the HAL is
        // stopped.
        invalidate();
        if (is_primary_) iface_tool_.lock()->SetWifiUpState(false);
        on_stop_complete_user_callback();
        is_started_ = false;
    };
    awaiting_event_loop_termination_ = true;
    global_func_table_.wifi_cleanup(global_handle_, onAsyncStopComplete);
    const auto status =
            stop_wait_cv_.wait_for(*lock, std::chrono::milliseconds(kMaxStopCompleteWaitMs),
                                   [this] { return !awaiting_event_loop_termination_; });
    if (!status) {
        LOG(ERROR) << "Legacy HAL stop failed or timed out";
        return WIFI_ERROR_UNKNOWN;
    }
    LOG(DEBUG) << "Legacy HAL stop complete";
    return WIFI_SUCCESS;
}

bool WifiLegacyHal::isStarted() {
    return is_started_;
}

wifi_error WifiLegacyHal::waitForDriverReady() {
    return global_func_table_.wifi_wait_for_driver_ready();
}

std::pair<wifi_error, std::string> WifiLegacyHal::getDriverVersion(const std::string& iface_name) {
    std::array<char, kMaxVersionStringLength> buffer;
    buffer.fill(0);
    wifi_error status = global_func_table_.wifi_get_driver_version(getIfaceHandle(iface_name),
                                                                   buffer.data(), buffer.size());
    return {status, buffer.data()};
}

std::pair<wifi_error, std::string> WifiLegacyHal::getFirmwareVersion(
        const std::string& iface_name) {
    std::array<char, kMaxVersionStringLength> buffer;
    buffer.fill(0);
    wifi_error status = global_func_table_.wifi_get_firmware_version(getIfaceHandle(iface_name),
                                                                     buffer.data(), buffer.size());
    return {status, buffer.data()};
}

std::pair<wifi_error, std::vector<uint8_t>> WifiLegacyHal::requestDriverMemoryDump(
        const std::string& iface_name) {
    std::vector<uint8_t> driver_dump;
    on_driver_memory_dump_internal_callback = [&driver_dump](char* buffer, int buffer_size) {
        driver_dump.insert(driver_dump.end(), reinterpret_cast<uint8_t*>(buffer),
                           reinterpret_cast<uint8_t*>(buffer) + buffer_size);
    };
    wifi_error status = global_func_table_.wifi_get_driver_memory_dump(getIfaceHandle(iface_name),
                                                                       {onSyncDriverMemoryDump});
    on_driver_memory_dump_internal_callback = nullptr;
    return {status, std::move(driver_dump)};
}

std::pair<wifi_error, std::vector<uint8_t>> WifiLegacyHal::requestFirmwareMemoryDump(
        const std::string& iface_name) {
    std::vector<uint8_t> firmware_dump;
    on_firmware_memory_dump_internal_callback = [&firmware_dump](char* buffer, int buffer_size) {
        firmware_dump.insert(firmware_dump.end(), reinterpret_cast<uint8_t*>(buffer),
                             reinterpret_cast<uint8_t*>(buffer) + buffer_size);
    };
    wifi_error status = global_func_table_.wifi_get_firmware_memory_dump(
            getIfaceHandle(iface_name), {onSyncFirmwareMemoryDump});
    on_firmware_memory_dump_internal_callback = nullptr;
    return {status, std::move(firmware_dump)};
}

std::pair<wifi_error, uint64_t> WifiLegacyHal::getSupportedFeatureSet(
        const std::string& iface_name) {
    feature_set set = 0, chip_set = 0;
    wifi_error status = WIFI_SUCCESS;

    static_assert(sizeof(set) == sizeof(uint64_t),
                  "Some feature_flags can not be represented in output");
    wifi_interface_handle iface_handle = getIfaceHandle(iface_name);

    global_func_table_.wifi_get_chip_feature_set(
            global_handle_, &chip_set); /* ignore error, chip_set will stay 0 */

    if (iface_handle) {
        status = global_func_table_.wifi_get_supported_feature_set(iface_handle, &set);
    }
    return {status, static_cast<uint64_t>(set | chip_set)};
}

std::pair<wifi_error, PacketFilterCapabilities> WifiLegacyHal::getPacketFilterCapabilities(
        const std::string& iface_name) {
    PacketFilterCapabilities caps;
    wifi_error status = global_func_table_.wifi_get_packet_filter_capabilities(
            getIfaceHandle(iface_name), &caps.version, &caps.max_len);
    return {status, caps};
}

wifi_error WifiLegacyHal::setPacketFilter(const std::string& iface_name,
                                          const std::vector<uint8_t>& program) {
    return global_func_table_.wifi_set_packet_filter(getIfaceHandle(iface_name), program.data(),
                                                     program.size());
}

std::pair<wifi_error, std::vector<uint8_t>> WifiLegacyHal::readApfPacketFilterData(
        const std::string& iface_name) {
    PacketFilterCapabilities caps;
    wifi_error status = global_func_table_.wifi_get_packet_filter_capabilities(
            getIfaceHandle(iface_name), &caps.version, &caps.max_len);
    if (status != WIFI_SUCCESS) {
        return {status, {}};
    }

    // Size the buffer to read the entire program & work memory.
    std::vector<uint8_t> buffer(caps.max_len);

    status = global_func_table_.wifi_read_packet_filter(
            getIfaceHandle(iface_name), /*src_offset=*/0, buffer.data(), buffer.size());
    return {status, std::move(buffer)};
}

std::pair<wifi_error, wifi_gscan_capabilities> WifiLegacyHal::getGscanCapabilities(
        const std::string& iface_name) {
    wifi_gscan_capabilities caps;
    wifi_error status =
            global_func_table_.wifi_get_gscan_capabilities(getIfaceHandle(iface_name), &caps);
    return {status, caps};
}

wifi_error WifiLegacyHal::startGscan(
        const std::string& iface_name, wifi_request_id id, const wifi_scan_cmd_params& params,
        const std::function<void(wifi_request_id)>& on_failure_user_callback,
        const on_gscan_results_callback& on_results_user_callback,
        const on_gscan_full_result_callback& on_full_result_user_callback) {
    // If there is already an ongoing background scan, reject new scan requests.
    if (on_gscan_event_internal_callback || on_gscan_full_result_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    // This callback will be used to either trigger |on_results_user_callback|
    // or |on_failure_user_callback|.
    on_gscan_event_internal_callback = [iface_name, on_failure_user_callback,
                                        on_results_user_callback,
                                        this](wifi_request_id id, wifi_scan_event event) {
        switch (event) {
            case WIFI_SCAN_RESULTS_AVAILABLE:
            case WIFI_SCAN_THRESHOLD_NUM_SCANS:
            case WIFI_SCAN_THRESHOLD_PERCENT: {
                wifi_error status;
                std::vector<wifi_cached_scan_results> cached_scan_results;
                std::tie(status, cached_scan_results) = getGscanCachedResults(iface_name);
                if (status == WIFI_SUCCESS) {
                    on_results_user_callback(id, cached_scan_results);
                    return;
                }
                FALLTHROUGH_INTENDED;
            }
            // Fall through if failed. Failure to retrieve cached scan
            // results should trigger a background scan failure.
            case WIFI_SCAN_FAILED:
                on_failure_user_callback(id);
                on_gscan_event_internal_callback = nullptr;
                on_gscan_full_result_internal_callback = nullptr;
                return;
        }
        LOG(FATAL) << "Unexpected gscan event received: " << event;
    };

    on_gscan_full_result_internal_callback = [on_full_result_user_callback](
                                                     wifi_request_id id, wifi_scan_result* result,
                                                     uint32_t buckets_scanned) {
        if (result) {
            on_full_result_user_callback(id, result, buckets_scanned);
        }
    };

    wifi_scan_result_handler handler = {onAsyncGscanFullResult, onAsyncGscanEvent};
    wifi_error status =
            global_func_table_.wifi_start_gscan(id, getIfaceHandle(iface_name), params, handler);
    if (status != WIFI_SUCCESS) {
        on_gscan_event_internal_callback = nullptr;
        on_gscan_full_result_internal_callback = nullptr;
    }
    return status;
}

wifi_error WifiLegacyHal::stopGscan(const std::string& iface_name, wifi_request_id id) {
    // If there is no an ongoing background scan, reject stop requests.
    // TODO(b/32337212): This needs to be handled by the HIDL object because we
    // need to return the NOT_STARTED error code.
    if (!on_gscan_event_internal_callback && !on_gscan_full_result_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    wifi_error status = global_func_table_.wifi_stop_gscan(id, getIfaceHandle(iface_name));
    // If the request Id is wrong, don't stop the ongoing background scan. Any
    // other error should be treated as the end of background scan.
    if (status != WIFI_ERROR_INVALID_REQUEST_ID) {
        on_gscan_event_internal_callback = nullptr;
        on_gscan_full_result_internal_callback = nullptr;
    }
    return status;
}

std::pair<wifi_error, std::vector<uint32_t>> WifiLegacyHal::getValidFrequenciesForBand(
        const std::string& iface_name, wifi_band band) {
    static_assert(sizeof(uint32_t) >= sizeof(wifi_channel),
                  "Wifi Channel cannot be represented in output");
    std::vector<uint32_t> freqs;
    freqs.resize(kMaxGscanFrequenciesForBand);
    int32_t num_freqs = 0;
    wifi_error status = global_func_table_.wifi_get_valid_channels(
            getIfaceHandle(iface_name), band, freqs.size(),
            reinterpret_cast<wifi_channel*>(freqs.data()), &num_freqs);
    CHECK(num_freqs >= 0 && static_cast<uint32_t>(num_freqs) <= kMaxGscanFrequenciesForBand);
    freqs.resize(num_freqs);
    return {status, std::move(freqs)};
}

wifi_error WifiLegacyHal::setDfsFlag(const std::string& iface_name, bool dfs_on) {
    return global_func_table_.wifi_set_nodfs_flag(getIfaceHandle(iface_name), dfs_on ? 0 : 1);
}

wifi_error WifiLegacyHal::enableLinkLayerStats(const std::string& iface_name, bool debug) {
    wifi_link_layer_params params;
    params.mpdu_size_threshold = kLinkLayerStatsDataMpduSizeThreshold;
    params.aggressive_statistics_gathering = debug;
    return global_func_table_.wifi_set_link_stats(getIfaceHandle(iface_name), params);
}

wifi_error WifiLegacyHal::disableLinkLayerStats(const std::string& iface_name) {
    // TODO: Do we care about these responses?
    uint32_t clear_mask_rsp;
    uint8_t stop_rsp;
    return global_func_table_.wifi_clear_link_stats(getIfaceHandle(iface_name), 0xFFFFFFFF,
                                                    &clear_mask_rsp, 1, &stop_rsp);
}

// Copies wifi_peer_info* to vector<WifiPeerInfo> and returns poiner to next element.
wifi_peer_info* WifiLegacyHal::copyPeerInfo(wifi_peer_info* peer_ptr,
                                            std::vector<WifiPeerInfo>& peers) {
    WifiPeerInfo peer;
    peer.peer_info = *peer_ptr;
    if (peer_ptr->num_rate > 0) {
        // Copy the rate stats.
        peer.rate_stats.assign(peer_ptr->rate_stats, peer_ptr->rate_stats + peer_ptr->num_rate);
    }
    peer.peer_info.num_rate = 0;
    // Push peer info.
    peers.push_back(peer);
    // Return the address of next peer info.
    return (wifi_peer_info*)((u8*)peer_ptr + sizeof(wifi_peer_info) +
                             (sizeof(wifi_rate_stat) * peer_ptr->num_rate));
}
// Copies wifi_link_stat* to vector<LinkStats> and returns poiner to next element.
wifi_link_stat* WifiLegacyHal::copyLinkStat(wifi_link_stat* stat_ptr,
                                            std::vector<LinkStats>& stats) {
    LinkStats linkStat;
    linkStat.stat = *stat_ptr;
    wifi_peer_info* l_peer_info_stats_ptr = stat_ptr->peer_info;
    for (uint32_t i = 0; i < linkStat.stat.num_peers; i++) {
        l_peer_info_stats_ptr = copyPeerInfo(l_peer_info_stats_ptr, linkStat.peers);
    }
    // Copied all peers to linkStat.peers.
    linkStat.stat.num_peers = 0;
    // Push link stat.
    stats.push_back(linkStat);
    // Read all peers, return the address of next wifi_link_stat.
    return (wifi_link_stat*)l_peer_info_stats_ptr;
}

wifi_error WifiLegacyHal::getLinkLayerStats(const std::string& iface_name,
                                            LinkLayerStats& link_stats,
                                            LinkLayerMlStats& link_ml_stats) {
    LinkLayerStats* link_stats_ptr = &link_stats;
    link_stats_ptr->valid = false;

    on_link_layer_stats_result_internal_callback = [&link_stats_ptr](
                                                           wifi_request_id /* id */,
                                                           wifi_iface_stat* iface_stats_ptr,
                                                           int num_radios,
                                                           wifi_radio_stat* radio_stats_ptr) {
        wifi_radio_stat* l_radio_stats_ptr;
        wifi_peer_info* l_peer_info_stats_ptr;
        link_stats_ptr->valid = true;

        if (iface_stats_ptr != nullptr) {
            link_stats_ptr->iface = *iface_stats_ptr;
            l_peer_info_stats_ptr = iface_stats_ptr->peer_info;
            for (uint32_t i = 0; i < iface_stats_ptr->num_peers; i++) {
                WifiPeerInfo peer;
                peer.peer_info = *l_peer_info_stats_ptr;
                if (l_peer_info_stats_ptr->num_rate > 0) {
                    /* Copy the rate stats */
                    peer.rate_stats.assign(
                            l_peer_info_stats_ptr->rate_stats,
                            l_peer_info_stats_ptr->rate_stats + l_peer_info_stats_ptr->num_rate);
                }
                peer.peer_info.num_rate = 0;
                link_stats_ptr->peers.push_back(peer);
                l_peer_info_stats_ptr =
                        (wifi_peer_info*)((u8*)l_peer_info_stats_ptr + sizeof(wifi_peer_info) +
                                          (sizeof(wifi_rate_stat) *
                                           l_peer_info_stats_ptr->num_rate));
            }
            link_stats_ptr->iface.num_peers = 0;
        } else {
            LOG(ERROR) << "Invalid iface stats in link layer stats";
        }
        if (num_radios <= 0 || radio_stats_ptr == nullptr) {
            LOG(ERROR) << "Invalid radio stats in link layer stats";
            return;
        }
        l_radio_stats_ptr = radio_stats_ptr;
        for (int i = 0; i < num_radios; i++) {
            LinkLayerRadioStats radio;

            radio.stats = *l_radio_stats_ptr;
            // Copy over the tx level array to the separate vector.
            if (l_radio_stats_ptr->num_tx_levels > 0 &&
                l_radio_stats_ptr->tx_time_per_levels != nullptr) {
                radio.tx_time_per_levels.assign(
                        l_radio_stats_ptr->tx_time_per_levels,
                        l_radio_stats_ptr->tx_time_per_levels + l_radio_stats_ptr->num_tx_levels);
            }
            radio.stats.num_tx_levels = 0;
            radio.stats.tx_time_per_levels = nullptr;
            /* Copy over the channel stat to separate vector */
            if (l_radio_stats_ptr->num_channels > 0) {
                /* Copy the channel stats */
                radio.channel_stats.assign(
                        l_radio_stats_ptr->channels,
                        l_radio_stats_ptr->channels + l_radio_stats_ptr->num_channels);
            }
            link_stats_ptr->radios.push_back(radio);
            l_radio_stats_ptr =
                    (wifi_radio_stat*)((u8*)l_radio_stats_ptr + sizeof(wifi_radio_stat) +
                                       (sizeof(wifi_channel_stat) *
                                        l_radio_stats_ptr->num_channels));
        }
    };

    LinkLayerMlStats* link_ml_stats_ptr = &link_ml_stats;
    link_ml_stats_ptr->valid = false;

    on_link_layer_ml_stats_result_internal_callback =
            [this, &link_ml_stats_ptr](wifi_request_id /* id */,
                                       wifi_iface_ml_stat* iface_ml_stats_ptr, int num_radios,
                                       wifi_radio_stat* radio_stats_ptr) {
                wifi_radio_stat* l_radio_stats_ptr;
                wifi_link_stat* l_link_stat_ptr;
                link_ml_stats_ptr->valid = true;

                if (iface_ml_stats_ptr != nullptr && iface_ml_stats_ptr->num_links > 0) {
                    // Copy stats from wifi_iface_ml_stat to LinkLayerMlStats,
                    //  - num_links * links[] to vector of links.
                    //  - num_peers * peer_info[] to vector of links[i].peers.
                    link_ml_stats_ptr->iface = *iface_ml_stats_ptr;
                    l_link_stat_ptr = iface_ml_stats_ptr->links;
                    for (int l = 0; l < iface_ml_stats_ptr->num_links; ++l) {
                        l_link_stat_ptr = copyLinkStat(l_link_stat_ptr, link_ml_stats_ptr->links);
                    }
                } else {
                    LOG(ERROR) << "Invalid iface stats in link layer stats";
                }
                if (num_radios <= 0 || radio_stats_ptr == nullptr) {
                    LOG(ERROR) << "Invalid radio stats in link layer stats";
                    return;
                }
                l_radio_stats_ptr = radio_stats_ptr;
                for (int i = 0; i < num_radios; i++) {
                    LinkLayerRadioStats radio;

                    radio.stats = *l_radio_stats_ptr;
                    // Copy over the tx level array to the separate vector.
                    if (l_radio_stats_ptr->num_tx_levels > 0 &&
                        l_radio_stats_ptr->tx_time_per_levels != nullptr) {
                        radio.tx_time_per_levels.assign(l_radio_stats_ptr->tx_time_per_levels,
                                                        l_radio_stats_ptr->tx_time_per_levels +
                                                                l_radio_stats_ptr->num_tx_levels);
                    }
                    radio.stats.num_tx_levels = 0;
                    radio.stats.tx_time_per_levels = nullptr;
                    /* Copy over the channel stat to separate vector */
                    if (l_radio_stats_ptr->num_channels > 0) {
                        /* Copy the channel stats */
                        radio.channel_stats.assign(
                                l_radio_stats_ptr->channels,
                                l_radio_stats_ptr->channels + l_radio_stats_ptr->num_channels);
                    }
                    link_ml_stats_ptr->radios.push_back(radio);
                    l_radio_stats_ptr =
                            (wifi_radio_stat*)((u8*)l_radio_stats_ptr + sizeof(wifi_radio_stat) +
                                               (sizeof(wifi_channel_stat) *
                                                l_radio_stats_ptr->num_channels));
                }
            };

    wifi_error status = global_func_table_.wifi_get_link_stats(
            0, getIfaceHandle(iface_name),
            {onSyncLinkLayerStatsResult, onSyncLinkLayerMlStatsResult});
    on_link_layer_stats_result_internal_callback = nullptr;
    on_link_layer_ml_stats_result_internal_callback = nullptr;

    return status;
}

wifi_error WifiLegacyHal::startRssiMonitoring(
        const std::string& iface_name, wifi_request_id id, int8_t max_rssi, int8_t min_rssi,
        const on_rssi_threshold_breached_callback& on_threshold_breached_user_callback) {
    if (on_rssi_threshold_breached_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    on_rssi_threshold_breached_internal_callback = [on_threshold_breached_user_callback](
                                                           wifi_request_id id, uint8_t* bssid_ptr,
                                                           int8_t rssi) {
        if (!bssid_ptr) {
            return;
        }
        std::array<uint8_t, ETH_ALEN> bssid_arr;
        // |bssid_ptr| pointer is assumed to have 6 bytes for the mac
        // address.
        std::copy(bssid_ptr, bssid_ptr + 6, std::begin(bssid_arr));
        on_threshold_breached_user_callback(id, bssid_arr, rssi);
    };
    wifi_error status = global_func_table_.wifi_start_rssi_monitoring(
            id, getIfaceHandle(iface_name), max_rssi, min_rssi, {onAsyncRssiThresholdBreached});
    if (status != WIFI_SUCCESS) {
        on_rssi_threshold_breached_internal_callback = nullptr;
    }
    return status;
}

wifi_error WifiLegacyHal::stopRssiMonitoring(const std::string& iface_name, wifi_request_id id) {
    if (!on_rssi_threshold_breached_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    wifi_error status =
            global_func_table_.wifi_stop_rssi_monitoring(id, getIfaceHandle(iface_name));
    // If the request Id is wrong, don't stop the ongoing rssi monitoring. Any
    // other error should be treated as the end of background scan.
    if (status != WIFI_ERROR_INVALID_REQUEST_ID) {
        on_rssi_threshold_breached_internal_callback = nullptr;
    }
    return status;
}

std::pair<wifi_error, wifi_roaming_capabilities> WifiLegacyHal::getRoamingCapabilities(
        const std::string& iface_name) {
    wifi_roaming_capabilities caps;
    wifi_error status =
            global_func_table_.wifi_get_roaming_capabilities(getIfaceHandle(iface_name), &caps);
    return {status, caps};
}

wifi_error WifiLegacyHal::configureRoaming(const std::string& iface_name,
                                           const wifi_roaming_config& config) {
    wifi_roaming_config config_internal = config;
    return global_func_table_.wifi_configure_roaming(getIfaceHandle(iface_name), &config_internal);
}

wifi_error WifiLegacyHal::enableFirmwareRoaming(const std::string& iface_name,
                                                fw_roaming_state_t state) {
    return global_func_table_.wifi_enable_firmware_roaming(getIfaceHandle(iface_name), state);
}

wifi_error WifiLegacyHal::configureNdOffload(const std::string& iface_name, bool enable) {
    return global_func_table_.wifi_configure_nd_offload(getIfaceHandle(iface_name), enable);
}

wifi_error WifiLegacyHal::startSendingOffloadedPacket(const std::string& iface_name, int32_t cmd_id,
                                                      uint16_t ether_type,
                                                      const std::vector<uint8_t>& ip_packet_data,
                                                      const std::array<uint8_t, 6>& src_address,
                                                      const std::array<uint8_t, 6>& dst_address,
                                                      int32_t period_in_ms) {
    std::vector<uint8_t> ip_packet_data_internal(ip_packet_data);
    std::vector<uint8_t> src_address_internal(src_address.data(),
                                              src_address.data() + src_address.size());
    std::vector<uint8_t> dst_address_internal(dst_address.data(),
                                              dst_address.data() + dst_address.size());
    return global_func_table_.wifi_start_sending_offloaded_packet(
            cmd_id, getIfaceHandle(iface_name), ether_type, ip_packet_data_internal.data(),
            ip_packet_data_internal.size(), src_address_internal.data(),
            dst_address_internal.data(), period_in_ms);
}

wifi_error WifiLegacyHal::stopSendingOffloadedPacket(const std::string& iface_name,
                                                     uint32_t cmd_id) {
    return global_func_table_.wifi_stop_sending_offloaded_packet(cmd_id,
                                                                 getIfaceHandle(iface_name));
}

wifi_error WifiLegacyHal::selectTxPowerScenario(const std::string& iface_name,
                                                wifi_power_scenario scenario) {
    return global_func_table_.wifi_select_tx_power_scenario(getIfaceHandle(iface_name), scenario);
}

wifi_error WifiLegacyHal::resetTxPowerScenario(const std::string& iface_name) {
    return global_func_table_.wifi_reset_tx_power_scenario(getIfaceHandle(iface_name));
}

wifi_error WifiLegacyHal::setLatencyMode(const std::string& iface_name, wifi_latency_mode mode) {
    return global_func_table_.wifi_set_latency_mode(getIfaceHandle(iface_name), mode);
}

wifi_error WifiLegacyHal::setThermalMitigationMode(wifi_thermal_mode mode,
                                                   uint32_t completion_window) {
    return global_func_table_.wifi_set_thermal_mitigation_mode(global_handle_, mode,
                                                               completion_window);
}

wifi_error WifiLegacyHal::setDscpToAccessCategoryMapping(uint32_t start, uint32_t end,
                                                         uint32_t access_category) {
    return global_func_table_.wifi_map_dscp_access_category(global_handle_, start, end,
                                                            access_category);
}

wifi_error WifiLegacyHal::resetDscpToAccessCategoryMapping() {
    return global_func_table_.wifi_reset_dscp_mapping(global_handle_);
}

std::pair<wifi_error, uint32_t> WifiLegacyHal::getLoggerSupportedFeatureSet(
        const std::string& iface_name) {
    uint32_t supported_feature_flags = 0;
    wifi_error status = WIFI_SUCCESS;

    wifi_interface_handle iface_handle = getIfaceHandle(iface_name);

    if (iface_handle) {
        status = global_func_table_.wifi_get_logger_supported_feature_set(iface_handle,
                                                                          &supported_feature_flags);
    }
    return {status, supported_feature_flags};
}

wifi_error WifiLegacyHal::startPktFateMonitoring(const std::string& iface_name) {
    return global_func_table_.wifi_start_pkt_fate_monitoring(getIfaceHandle(iface_name));
}

std::pair<wifi_error, std::vector<wifi_tx_report>> WifiLegacyHal::getTxPktFates(
        const std::string& iface_name) {
    std::vector<wifi_tx_report> tx_pkt_fates;
    tx_pkt_fates.resize(MAX_FATE_LOG_LEN);
    size_t num_fates = 0;
    wifi_error status = global_func_table_.wifi_get_tx_pkt_fates(
            getIfaceHandle(iface_name), tx_pkt_fates.data(), tx_pkt_fates.size(), &num_fates);
    CHECK(num_fates <= MAX_FATE_LOG_LEN);
    tx_pkt_fates.resize(num_fates);
    return {status, std::move(tx_pkt_fates)};
}

std::pair<wifi_error, std::vector<wifi_rx_report>> WifiLegacyHal::getRxPktFates(
        const std::string& iface_name) {
    std::vector<wifi_rx_report> rx_pkt_fates;
    rx_pkt_fates.resize(MAX_FATE_LOG_LEN);
    size_t num_fates = 0;
    wifi_error status = global_func_table_.wifi_get_rx_pkt_fates(
            getIfaceHandle(iface_name), rx_pkt_fates.data(), rx_pkt_fates.size(), &num_fates);
    CHECK(num_fates <= MAX_FATE_LOG_LEN);
    rx_pkt_fates.resize(num_fates);
    return {status, std::move(rx_pkt_fates)};
}

std::pair<wifi_error, WakeReasonStats> WifiLegacyHal::getWakeReasonStats(
        const std::string& iface_name) {
    WakeReasonStats stats;
    stats.cmd_event_wake_cnt.resize(kMaxWakeReasonStatsArraySize);
    stats.driver_fw_local_wake_cnt.resize(kMaxWakeReasonStatsArraySize);

    // This legacy struct needs separate memory to store the variable sized wake
    // reason types.
    stats.wake_reason_cnt.cmd_event_wake_cnt =
            reinterpret_cast<int32_t*>(stats.cmd_event_wake_cnt.data());
    stats.wake_reason_cnt.cmd_event_wake_cnt_sz = stats.cmd_event_wake_cnt.size();
    stats.wake_reason_cnt.cmd_event_wake_cnt_used = 0;
    stats.wake_reason_cnt.driver_fw_local_wake_cnt =
            reinterpret_cast<int32_t*>(stats.driver_fw_local_wake_cnt.data());
    stats.wake_reason_cnt.driver_fw_local_wake_cnt_sz = stats.driver_fw_local_wake_cnt.size();
    stats.wake_reason_cnt.driver_fw_local_wake_cnt_used = 0;

    wifi_error status = global_func_table_.wifi_get_wake_reason_stats(getIfaceHandle(iface_name),
                                                                      &stats.wake_reason_cnt);

    CHECK(stats.wake_reason_cnt.cmd_event_wake_cnt_used >= 0 &&
          static_cast<uint32_t>(stats.wake_reason_cnt.cmd_event_wake_cnt_used) <=
                  kMaxWakeReasonStatsArraySize);
    stats.cmd_event_wake_cnt.resize(stats.wake_reason_cnt.cmd_event_wake_cnt_used);
    stats.wake_reason_cnt.cmd_event_wake_cnt = nullptr;

    CHECK(stats.wake_reason_cnt.driver_fw_local_wake_cnt_used >= 0 &&
          static_cast<uint32_t>(stats.wake_reason_cnt.driver_fw_local_wake_cnt_used) <=
                  kMaxWakeReasonStatsArraySize);
    stats.driver_fw_local_wake_cnt.resize(stats.wake_reason_cnt.driver_fw_local_wake_cnt_used);
    stats.wake_reason_cnt.driver_fw_local_wake_cnt = nullptr;

    return {status, stats};
}

wifi_error WifiLegacyHal::registerRingBufferCallbackHandler(
        const std::string& iface_name, const on_ring_buffer_data_callback& on_user_data_callback) {
    if (on_ring_buffer_data_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    on_ring_buffer_data_internal_callback = [on_user_data_callback](
                                                    char* ring_name, char* buffer, int buffer_size,
                                                    wifi_ring_buffer_status* status) {
        if (status && buffer) {
            std::vector<uint8_t> buffer_vector(reinterpret_cast<uint8_t*>(buffer),
                                               reinterpret_cast<uint8_t*>(buffer) + buffer_size);
            on_user_data_callback(ring_name, buffer_vector, *status);
        }
    };
    wifi_error status = global_func_table_.wifi_set_log_handler(0, getIfaceHandle(iface_name),
                                                                {onAsyncRingBufferData});
    if (status != WIFI_SUCCESS) {
        on_ring_buffer_data_internal_callback = nullptr;
    }
    return status;
}

wifi_error WifiLegacyHal::deregisterRingBufferCallbackHandler(const std::string& iface_name) {
    if (!on_ring_buffer_data_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    on_ring_buffer_data_internal_callback = nullptr;
    return global_func_table_.wifi_reset_log_handler(0, getIfaceHandle(iface_name));
}

std::pair<wifi_error, std::vector<wifi_ring_buffer_status>> WifiLegacyHal::getRingBuffersStatus(
        const std::string& iface_name) {
    std::vector<wifi_ring_buffer_status> ring_buffers_status;
    ring_buffers_status.resize(kMaxRingBuffers);
    uint32_t num_rings = kMaxRingBuffers;
    wifi_error status = global_func_table_.wifi_get_ring_buffers_status(
            getIfaceHandle(iface_name), &num_rings, ring_buffers_status.data());
    CHECK(num_rings <= kMaxRingBuffers);
    ring_buffers_status.resize(num_rings);
    return {status, std::move(ring_buffers_status)};
}

wifi_error WifiLegacyHal::startRingBufferLogging(const std::string& iface_name,
                                                 const std::string& ring_name,
                                                 uint32_t verbose_level, uint32_t max_interval_sec,
                                                 uint32_t min_data_size) {
    return global_func_table_.wifi_start_logging(getIfaceHandle(iface_name), verbose_level, 0,
                                                 max_interval_sec, min_data_size,
                                                 makeCharVec(ring_name).data());
}

wifi_error WifiLegacyHal::getRingBufferData(const std::string& iface_name,
                                            const std::string& ring_name) {
    return global_func_table_.wifi_get_ring_data(getIfaceHandle(iface_name),
                                                 makeCharVec(ring_name).data());
}

wifi_error WifiLegacyHal::registerErrorAlertCallbackHandler(
        const std::string& iface_name, const on_error_alert_callback& on_user_alert_callback) {
    if (on_error_alert_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    on_error_alert_internal_callback = [on_user_alert_callback](wifi_request_id id, char* buffer,
                                                                int buffer_size, int err_code) {
        if (buffer) {
            CHECK(id == 0);
            on_user_alert_callback(
                    err_code,
                    std::vector<uint8_t>(reinterpret_cast<uint8_t*>(buffer),
                                         reinterpret_cast<uint8_t*>(buffer) + buffer_size));
        }
    };
    wifi_error status = global_func_table_.wifi_set_alert_handler(0, getIfaceHandle(iface_name),
                                                                  {onAsyncErrorAlert});
    if (status != WIFI_SUCCESS) {
        on_error_alert_internal_callback = nullptr;
    }
    return status;
}

wifi_error WifiLegacyHal::deregisterErrorAlertCallbackHandler(const std::string& iface_name) {
    if (!on_error_alert_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    on_error_alert_internal_callback = nullptr;
    return global_func_table_.wifi_reset_alert_handler(0, getIfaceHandle(iface_name));
}

wifi_error WifiLegacyHal::registerRadioModeChangeCallbackHandler(
        const std::string& iface_name,
        const on_radio_mode_change_callback& on_user_change_callback) {
    if (on_radio_mode_change_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    on_radio_mode_change_internal_callback = [on_user_change_callback](
                                                     wifi_request_id /* id */, uint32_t num_macs,
                                                     wifi_mac_info* mac_infos_arr) {
        if (num_macs > 0 && mac_infos_arr) {
            std::vector<WifiMacInfo> mac_infos_vec;
            for (uint32_t i = 0; i < num_macs; i++) {
                WifiMacInfo mac_info;
                mac_info.wlan_mac_id = mac_infos_arr[i].wlan_mac_id;
                mac_info.mac_band = mac_infos_arr[i].mac_band;
                for (int32_t j = 0; j < mac_infos_arr[i].num_iface; j++) {
                    WifiIfaceInfo iface_info;
                    iface_info.name = mac_infos_arr[i].iface_info[j].iface_name;
                    iface_info.channel = mac_infos_arr[i].iface_info[j].channel;
                    mac_info.iface_infos.push_back(iface_info);
                }
                mac_infos_vec.push_back(mac_info);
            }
            on_user_change_callback(mac_infos_vec);
        }
    };
    wifi_error status = global_func_table_.wifi_set_radio_mode_change_handler(
            0, getIfaceHandle(iface_name), {onAsyncRadioModeChange});
    if (status != WIFI_SUCCESS) {
        on_radio_mode_change_internal_callback = nullptr;
    }
    return status;
}

wifi_error WifiLegacyHal::registerSubsystemRestartCallbackHandler(
        const on_subsystem_restart_callback& on_restart_callback) {
    if (on_subsystem_restart_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    on_subsystem_restart_internal_callback = [on_restart_callback](const char* error) {
        on_restart_callback(error);
    };
    wifi_error status = global_func_table_.wifi_set_subsystem_restart_handler(
            global_handle_, {onAsyncSubsystemRestart});
    if (status != WIFI_SUCCESS) {
        on_subsystem_restart_internal_callback = nullptr;
    }
    return status;
}

wifi_error WifiLegacyHal::startRttRangeRequestV4(
        const std::string& iface_name, wifi_request_id id,
        const std::vector<wifi_rtt_config_v4>& rtt_configs,
        const on_rtt_results_callback_v4& on_results_user_callback_v4) {
    if (on_rtt_results_internal_callback_v4) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    on_rtt_results_internal_callback_v4 = [on_results_user_callback_v4](
                                                  wifi_request_id id, unsigned num_results,
                                                  wifi_rtt_result_v4* rtt_results_v4[]) {
        if (num_results > 0 && !rtt_results_v4) {
            LOG(ERROR) << "Unexpected nullptr in RTT v4 results";
            return;
        }
        std::vector<const wifi_rtt_result_v4*> rtt_results_vec_v4;
        std::copy_if(rtt_results_v4, rtt_results_v4 + num_results,
                     back_inserter(rtt_results_vec_v4),
                     [](wifi_rtt_result_v4* rtt_result_v4) { return rtt_result_v4 != nullptr; });
        on_results_user_callback_v4(id, rtt_results_vec_v4);
    };

    std::vector<wifi_rtt_config_v4> rtt_configs_internal(rtt_configs);
    wifi_error status = global_func_table_.wifi_rtt_range_request_v4(
            id, getIfaceHandle(iface_name), rtt_configs.size(), rtt_configs_internal.data(),
            {onAsyncRttResultsV4});
    if (status != WIFI_SUCCESS) {
        invalidateRttResultsCallbacks();
    }
    return status;
}

wifi_error WifiLegacyHal::startRttRangeRequestV3(
        const std::string& iface_name, wifi_request_id id,
        const std::vector<wifi_rtt_config_v3>& rtt_configs,
        const on_rtt_results_callback_v3& on_results_user_callback_v3) {
    if (on_rtt_results_internal_callback_v3) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    on_rtt_results_internal_callback_v3 = [on_results_user_callback_v3](
                                                  wifi_request_id id, unsigned num_results,
                                                  wifi_rtt_result_v3* rtt_results_v3[]) {
        if (num_results > 0 && !rtt_results_v3) {
            LOG(ERROR) << "Unexpected nullptr in RTT v3 results";
            return;
        }
        std::vector<const wifi_rtt_result_v3*> rtt_results_vec_v3;
        std::copy_if(rtt_results_v3, rtt_results_v3 + num_results,
                     back_inserter(rtt_results_vec_v3),
                     [](wifi_rtt_result_v3* rtt_result_v3) { return rtt_result_v3 != nullptr; });
        on_results_user_callback_v3(id, rtt_results_vec_v3);
    };

    std::vector<wifi_rtt_config_v3> rtt_configs_internal(rtt_configs);
    wifi_error status = global_func_table_.wifi_rtt_range_request_v3(
            id, getIfaceHandle(iface_name), rtt_configs.size(), rtt_configs_internal.data(),
            {onAsyncRttResultsV3});
    if (status != WIFI_SUCCESS) {
        invalidateRttResultsCallbacks();
    }
    return status;
}

wifi_error WifiLegacyHal::startRttRangeRequest(
        const std::string& iface_name, wifi_request_id id,
        const std::vector<wifi_rtt_config>& rtt_configs,
        const on_rtt_results_callback& on_results_user_callback,
        const on_rtt_results_callback_v2& on_results_user_callback_v2) {
    if (on_rtt_results_internal_callback || on_rtt_results_internal_callback_v2) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    on_rtt_results_internal_callback = [on_results_user_callback](wifi_request_id id,
                                                                  unsigned num_results,
                                                                  wifi_rtt_result* rtt_results[]) {
        if (num_results > 0 && !rtt_results) {
            LOG(ERROR) << "Unexpected nullptr in RTT results";
            return;
        }
        std::vector<const wifi_rtt_result*> rtt_results_vec;
        std::copy_if(rtt_results, rtt_results + num_results, back_inserter(rtt_results_vec),
                     [](wifi_rtt_result* rtt_result) { return rtt_result != nullptr; });
        on_results_user_callback(id, rtt_results_vec);
    };

    on_rtt_results_internal_callback_v2 = [on_results_user_callback_v2](
                                                  wifi_request_id id, unsigned num_results,
                                                  wifi_rtt_result_v2* rtt_results_v2[]) {
        if (num_results > 0 && !rtt_results_v2) {
            LOG(ERROR) << "Unexpected nullptr in RTT results";
            return;
        }
        std::vector<const wifi_rtt_result_v2*> rtt_results_vec_v2;
        std::copy_if(rtt_results_v2, rtt_results_v2 + num_results,
                     back_inserter(rtt_results_vec_v2),
                     [](wifi_rtt_result_v2* rtt_result_v2) { return rtt_result_v2 != nullptr; });
        on_results_user_callback_v2(id, rtt_results_vec_v2);
    };

    std::vector<wifi_rtt_config> rtt_configs_internal(rtt_configs);
    wifi_error status = global_func_table_.wifi_rtt_range_request(
            id, getIfaceHandle(iface_name), rtt_configs.size(), rtt_configs_internal.data(),
            {onAsyncRttResults, onAsyncRttResultsV2});
    if (status != WIFI_SUCCESS) {
        invalidateRttResultsCallbacks();
    }
    return status;
}

wifi_error WifiLegacyHal::cancelRttRangeRequest(
        const std::string& iface_name, wifi_request_id id,
        const std::vector<std::array<uint8_t, ETH_ALEN>>& mac_addrs) {
    if (!on_rtt_results_internal_callback && !on_rtt_results_internal_callback_v2) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    static_assert(sizeof(mac_addr) == sizeof(std::array<uint8_t, ETH_ALEN>),
                  "MAC address size mismatch");
    // TODO: How do we handle partial cancels (i.e only a subset of enabled mac
    // addressed are cancelled).
    std::vector<std::array<uint8_t, ETH_ALEN>> mac_addrs_internal(mac_addrs);
    wifi_error status = global_func_table_.wifi_rtt_range_cancel(
            id, getIfaceHandle(iface_name), mac_addrs.size(),
            reinterpret_cast<mac_addr*>(mac_addrs_internal.data()));
    // If the request Id is wrong, don't stop the ongoing range request. Any
    // other error should be treated as the end of rtt ranging.
    if (status != WIFI_ERROR_INVALID_REQUEST_ID) {
        invalidateRttResultsCallbacks();
    }
    return status;
}

std::pair<wifi_error, wifi_rtt_capabilities> WifiLegacyHal::getRttCapabilities(
        const std::string& iface_name) {
    wifi_rtt_capabilities rtt_caps;
    wifi_error status =
            global_func_table_.wifi_get_rtt_capabilities(getIfaceHandle(iface_name), &rtt_caps);
    return {status, rtt_caps};
}

std::pair<wifi_error, wifi_rtt_capabilities_v3> WifiLegacyHal::getRttCapabilitiesV3(
        const std::string& iface_name) {
    wifi_rtt_capabilities_v3 rtt_caps_v3;
    wifi_error status = global_func_table_.wifi_get_rtt_capabilities_v3(getIfaceHandle(iface_name),
                                                                        &rtt_caps_v3);
    return {status, rtt_caps_v3};
}

std::pair<wifi_error, wifi_rtt_capabilities_v4> WifiLegacyHal::getRttCapabilitiesV4(
        const std::string& iface_name) {
    wifi_rtt_capabilities_v4 rtt_caps_v4;
    wifi_error status = global_func_table_.wifi_get_rtt_capabilities_v4(getIfaceHandle(iface_name),
                                                                        &rtt_caps_v4);
    return {status, rtt_caps_v4};
}

std::pair<wifi_error, wifi_rtt_responder> WifiLegacyHal::getRttResponderInfo(
        const std::string& iface_name) {
    wifi_rtt_responder rtt_responder;
    wifi_error status = global_func_table_.wifi_rtt_get_responder_info(getIfaceHandle(iface_name),
                                                                       &rtt_responder);
    return {status, rtt_responder};
}

wifi_error WifiLegacyHal::enableRttResponder(const std::string& iface_name, wifi_request_id id,
                                             const wifi_channel_info& channel_hint,
                                             uint32_t max_duration_secs,
                                             const wifi_rtt_responder& info) {
    wifi_rtt_responder info_internal(info);
    return global_func_table_.wifi_enable_responder(id, getIfaceHandle(iface_name), channel_hint,
                                                    max_duration_secs, &info_internal);
}

wifi_error WifiLegacyHal::disableRttResponder(const std::string& iface_name, wifi_request_id id) {
    return global_func_table_.wifi_disable_responder(id, getIfaceHandle(iface_name));
}

wifi_error WifiLegacyHal::setRttLci(const std::string& iface_name, wifi_request_id id,
                                    const wifi_lci_information& info) {
    wifi_lci_information info_internal(info);
    return global_func_table_.wifi_set_lci(id, getIfaceHandle(iface_name), &info_internal);
}

wifi_error WifiLegacyHal::setRttLcr(const std::string& iface_name, wifi_request_id id,
                                    const wifi_lcr_information& info) {
    wifi_lcr_information info_internal(info);
    return global_func_table_.wifi_set_lcr(id, getIfaceHandle(iface_name), &info_internal);
}

wifi_error WifiLegacyHal::nanRegisterCallbackHandlers(const std::string& iface_name,
                                                      const NanCallbackHandlers& user_callbacks) {
    on_nan_notify_response_user_callback = user_callbacks.on_notify_response;
    on_nan_event_publish_terminated_user_callback = user_callbacks.on_event_publish_terminated;
    on_nan_event_match_user_callback = user_callbacks.on_event_match;
    on_nan_event_match_expired_user_callback = user_callbacks.on_event_match_expired;
    on_nan_event_subscribe_terminated_user_callback = user_callbacks.on_event_subscribe_terminated;
    on_nan_event_followup_user_callback = user_callbacks.on_event_followup;
    on_nan_event_disc_eng_event_user_callback = user_callbacks.on_event_disc_eng_event;
    on_nan_event_disabled_user_callback = user_callbacks.on_event_disabled;
    on_nan_event_tca_user_callback = user_callbacks.on_event_tca;
    on_nan_event_beacon_sdf_payload_user_callback = user_callbacks.on_event_beacon_sdf_payload;
    on_nan_event_data_path_request_user_callback = user_callbacks.on_event_data_path_request;
    on_nan_event_pairing_request_user_callback = user_callbacks.on_event_pairing_request;
    on_nan_event_pairing_confirm_user_callback = user_callbacks.on_event_pairing_confirm;
    on_nan_event_bootstrapping_request_user_callback =
            user_callbacks.on_event_bootstrapping_request;
    on_nan_event_bootstrapping_confirm_user_callback =
            user_callbacks.on_event_bootstrapping_confirm;
    on_nan_event_data_path_confirm_user_callback = user_callbacks.on_event_data_path_confirm;
    on_nan_event_data_path_end_user_callback = user_callbacks.on_event_data_path_end;
    on_nan_event_transmit_follow_up_user_callback = user_callbacks.on_event_transmit_follow_up;
    on_nan_event_range_request_user_callback = user_callbacks.on_event_range_request;
    on_nan_event_range_report_user_callback = user_callbacks.on_event_range_report;
    on_nan_event_schedule_update_user_callback = user_callbacks.on_event_schedule_update;
    on_nan_event_suspension_mode_change_user_callback =
            user_callbacks.on_event_suspension_mode_change;
    on_nan_event_ranging_results_callback = user_callbacks.on_ranging_results;

    return global_func_table_.wifi_nan_register_handler(getIfaceHandle(iface_name),
                                                        {onAsyncNanNotifyResponse,
                                                         onAsyncNanEventPublishReplied,
                                                         onAsyncNanEventPublishTerminated,
                                                         onAsyncNanEventMatch,
                                                         onAsyncNanEventMatchExpired,
                                                         onAsyncNanEventSubscribeTerminated,
                                                         onAsyncNanEventFollowup,
                                                         onAsyncNanEventDiscEngEvent,
                                                         onAsyncNanEventDisabled,
                                                         onAsyncNanEventTca,
                                                         onAsyncNanEventBeaconSdfPayload,
                                                         onAsyncNanEventDataPathRequest,
                                                         onAsyncNanEventDataPathConfirm,
                                                         onAsyncNanEventDataPathEnd,
                                                         onAsyncNanEventTransmitFollowUp,
                                                         onAsyncNanEventRangeRequest,
                                                         onAsyncNanEventRangeReport,
                                                         onAsyncNanEventScheduleUpdate,
                                                         onAsyncNanEventPairingRequest,
                                                         onAsyncNanEventPairingConfirm,
                                                         onAsyncNanEventBootstrappingRequest,
                                                         onAsyncNanEventBootstrappingConfirm,
                                                         onAsyncNanEventSuspensionModeChange,
                                                         onAsyncNanEventRangingResults});
}

wifi_error WifiLegacyHal::nanEnableRequest(const std::string& iface_name, transaction_id id,
                                           const NanEnableRequest& msg) {
    NanEnableRequest msg_internal(msg);
    return global_func_table_.wifi_nan_enable_request(id, getIfaceHandle(iface_name),
                                                      &msg_internal);
}

wifi_error WifiLegacyHal::nanDisableRequest(const std::string& iface_name, transaction_id id) {
    return global_func_table_.wifi_nan_disable_request(id, getIfaceHandle(iface_name));
}

wifi_error WifiLegacyHal::nanPublishRequest(const std::string& iface_name, transaction_id id,
                                            const NanPublishRequest& msg) {
    NanPublishRequest msg_internal(msg);
    return global_func_table_.wifi_nan_publish_request(id, getIfaceHandle(iface_name),
                                                       &msg_internal);
}

wifi_error WifiLegacyHal::nanPublishCancelRequest(const std::string& iface_name, transaction_id id,
                                                  const NanPublishCancelRequest& msg) {
    NanPublishCancelRequest msg_internal(msg);
    return global_func_table_.wifi_nan_publish_cancel_request(id, getIfaceHandle(iface_name),
                                                              &msg_internal);
}

wifi_error WifiLegacyHal::nanSubscribeRequest(const std::string& iface_name, transaction_id id,
                                              const NanSubscribeRequest& msg) {
    NanSubscribeRequest msg_internal(msg);
    return global_func_table_.wifi_nan_subscribe_request(id, getIfaceHandle(iface_name),
                                                         &msg_internal);
}

wifi_error WifiLegacyHal::nanSubscribeCancelRequest(const std::string& iface_name,
                                                    transaction_id id,
                                                    const NanSubscribeCancelRequest& msg) {
    NanSubscribeCancelRequest msg_internal(msg);
    return global_func_table_.wifi_nan_subscribe_cancel_request(id, getIfaceHandle(iface_name),
                                                                &msg_internal);
}

wifi_error WifiLegacyHal::nanTransmitFollowupRequest(const std::string& iface_name,
                                                     transaction_id id,
                                                     const NanTransmitFollowupRequest& msg) {
    NanTransmitFollowupRequest msg_internal(msg);
    return global_func_table_.wifi_nan_transmit_followup_request(id, getIfaceHandle(iface_name),
                                                                 &msg_internal);
}

wifi_error WifiLegacyHal::nanStatsRequest(const std::string& iface_name, transaction_id id,
                                          const NanStatsRequest& msg) {
    NanStatsRequest msg_internal(msg);
    return global_func_table_.wifi_nan_stats_request(id, getIfaceHandle(iface_name), &msg_internal);
}

wifi_error WifiLegacyHal::nanConfigRequest(const std::string& iface_name, transaction_id id,
                                           const NanConfigRequest& msg) {
    NanConfigRequest msg_internal(msg);
    return global_func_table_.wifi_nan_config_request(id, getIfaceHandle(iface_name),
                                                      &msg_internal);
}

wifi_error WifiLegacyHal::nanTcaRequest(const std::string& iface_name, transaction_id id,
                                        const NanTCARequest& msg) {
    NanTCARequest msg_internal(msg);
    return global_func_table_.wifi_nan_tca_request(id, getIfaceHandle(iface_name), &msg_internal);
}

wifi_error WifiLegacyHal::nanBeaconSdfPayloadRequest(const std::string& iface_name,
                                                     transaction_id id,
                                                     const NanBeaconSdfPayloadRequest& msg) {
    NanBeaconSdfPayloadRequest msg_internal(msg);
    return global_func_table_.wifi_nan_beacon_sdf_payload_request(id, getIfaceHandle(iface_name),
                                                                  &msg_internal);
}

std::pair<wifi_error, NanVersion> WifiLegacyHal::nanGetVersion() {
    NanVersion version;
    wifi_error status = global_func_table_.wifi_nan_get_version(global_handle_, &version);
    return {status, version};
}

wifi_error WifiLegacyHal::nanGetCapabilities(const std::string& iface_name, transaction_id id) {
    return global_func_table_.wifi_nan_get_capabilities(id, getIfaceHandle(iface_name));
}

wifi_error WifiLegacyHal::nanDataInterfaceCreate(const std::string& iface_name, transaction_id id,
                                                 const std::string& data_iface_name) {
    return global_func_table_.wifi_nan_data_interface_create(id, getIfaceHandle(iface_name),
                                                             makeCharVec(data_iface_name).data());
}

wifi_error WifiLegacyHal::nanDataInterfaceDelete(const std::string& iface_name, transaction_id id,
                                                 const std::string& data_iface_name) {
    return global_func_table_.wifi_nan_data_interface_delete(id, getIfaceHandle(iface_name),
                                                             makeCharVec(data_iface_name).data());
}

wifi_error WifiLegacyHal::nanDataRequestInitiator(const std::string& iface_name, transaction_id id,
                                                  const NanDataPathInitiatorRequest& msg) {
    NanDataPathInitiatorRequest msg_internal(msg);
    return global_func_table_.wifi_nan_data_request_initiator(id, getIfaceHandle(iface_name),
                                                              &msg_internal);
}

wifi_error WifiLegacyHal::nanDataIndicationResponse(const std::string& iface_name,
                                                    transaction_id id,
                                                    const NanDataPathIndicationResponse& msg) {
    NanDataPathIndicationResponse msg_internal(msg);
    return global_func_table_.wifi_nan_data_indication_response(id, getIfaceHandle(iface_name),
                                                                &msg_internal);
}

wifi_error WifiLegacyHal::nanPairingRequest(const std::string& iface_name, transaction_id id,
                                            const NanPairingRequest& msg) {
    NanPairingRequest msg_internal(msg);
    return global_func_table_.wifi_nan_pairing_request(id, getIfaceHandle(iface_name),
                                                       &msg_internal);
}

wifi_error WifiLegacyHal::nanPairingIndicationResponse(const std::string& iface_name,
                                                       transaction_id id,
                                                       const NanPairingIndicationResponse& msg) {
    NanPairingIndicationResponse msg_internal(msg);
    return global_func_table_.wifi_nan_pairing_indication_response(id, getIfaceHandle(iface_name),
                                                                   &msg_internal);
}

wifi_error WifiLegacyHal::nanBootstrappingRequest(const std::string& iface_name, transaction_id id,
                                                  const NanBootstrappingRequest& msg) {
    NanBootstrappingRequest msg_internal(msg);
    return global_func_table_.wifi_nan_bootstrapping_request(id, getIfaceHandle(iface_name),
                                                             &msg_internal);
}

wifi_error WifiLegacyHal::nanBootstrappingIndicationResponse(
        const std::string& iface_name, transaction_id id,
        const NanBootstrappingIndicationResponse& msg) {
    NanBootstrappingIndicationResponse msg_internal(msg);
    return global_func_table_.wifi_nan_bootstrapping_indication_response(
            id, getIfaceHandle(iface_name), &msg_internal);
}

typedef struct {
    u8 num_ndp_instances;
    NanDataPathId ndp_instance_id;
} NanDataPathEndSingleNdpIdRequest;

wifi_error WifiLegacyHal::nanDataEnd(const std::string& iface_name, transaction_id id,
                                     uint32_t ndpInstanceId) {
    NanDataPathEndSingleNdpIdRequest msg;
    msg.num_ndp_instances = 1;
    msg.ndp_instance_id = ndpInstanceId;
    wifi_error status = global_func_table_.wifi_nan_data_end(id, getIfaceHandle(iface_name),
                                                             (NanDataPathEndRequest*)&msg);
    return status;
}

wifi_error WifiLegacyHal::nanPairingEnd(const std::string& iface_name, transaction_id id,
                                        uint32_t pairingId) {
    NanPairingEndRequest msg;
    msg.pairing_instance_id = pairingId;
    wifi_error status =
            global_func_table_.wifi_nan_pairing_end(id, getIfaceHandle(iface_name), &msg);
    return status;
}

wifi_error WifiLegacyHal::nanSuspendRequest(const std::string& iface_name, transaction_id id,
                                            const NanSuspendRequest& msg) {
    NanSuspendRequest msg_internal(msg);
    wifi_error status = global_func_table_.wifi_nan_suspend_request(id, getIfaceHandle(iface_name),
                                                                    &msg_internal);
    return status;
}

wifi_error WifiLegacyHal::nanResumeRequest(const std::string& iface_name, transaction_id id,
                                           const NanResumeRequest& msg) {
    NanResumeRequest msg_internal(msg);
    wifi_error status = global_func_table_.wifi_nan_resume_request(id, getIfaceHandle(iface_name),
                                                                   &msg_internal);
    return status;
}

wifi_error WifiLegacyHal::setCountryCode(const std::string& iface_name,
                                         const std::array<uint8_t, 2> code) {
    std::string code_str(code.data(), code.data() + code.size());
    return global_func_table_.wifi_set_country_code(getIfaceHandle(iface_name), code_str.c_str());
}

wifi_error WifiLegacyHal::retrieveIfaceHandles() {
    wifi_interface_handle* iface_handles = nullptr;
    int num_iface_handles = 0;
    wifi_error status =
            global_func_table_.wifi_get_ifaces(global_handle_, &num_iface_handles, &iface_handles);
    if (status != WIFI_SUCCESS) {
        LOG(ERROR) << "Failed to enumerate interface handles";
        return status;
    }
    iface_name_to_handle_.clear();
    for (int i = 0; i < num_iface_handles; ++i) {
        std::array<char, IFNAMSIZ> iface_name_arr = {};
        status = global_func_table_.wifi_get_iface_name(iface_handles[i], iface_name_arr.data(),
                                                        iface_name_arr.size());
        if (status != WIFI_SUCCESS) {
            LOG(WARNING) << "Failed to get interface handle name";
            continue;
        }
        // Assuming the interface name is null terminated since the legacy HAL
        // API does not return a size.
        std::string iface_name(iface_name_arr.data());
        LOG(INFO) << "Adding interface handle for " << iface_name;
        iface_name_to_handle_[iface_name] = iface_handles[i];
    }
    return WIFI_SUCCESS;
}

wifi_interface_handle WifiLegacyHal::getIfaceHandle(const std::string& iface_name) {
    const auto iface_handle_iter = iface_name_to_handle_.find(iface_name);
    if (iface_handle_iter == iface_name_to_handle_.end()) {
        LOG(ERROR) << "Unknown iface name: " << iface_name;
        return nullptr;
    }
    return iface_handle_iter->second;
}

void WifiLegacyHal::runEventLoop() {
    LOG(DEBUG) << "Starting legacy HAL event loop";
    global_func_table_.wifi_event_loop(global_handle_);
    const auto lock = aidl_sync_util::acquireGlobalLock();
    if (!awaiting_event_loop_termination_) {
        LOG(FATAL) << "Legacy HAL event loop terminated, but HAL was not stopping";
    }
    LOG(DEBUG) << "Legacy HAL event loop terminated";
    awaiting_event_loop_termination_ = false;
    stop_wait_cv_.notify_one();
}

std::pair<wifi_error, std::vector<wifi_cached_scan_results>> WifiLegacyHal::getGscanCachedResults(
        const std::string& iface_name) {
    std::vector<wifi_cached_scan_results> cached_scan_results;
    cached_scan_results.resize(kMaxCachedGscanResults);
    int32_t num_results = 0;
    wifi_error status = global_func_table_.wifi_get_cached_gscan_results(
            getIfaceHandle(iface_name), true /* always flush */, cached_scan_results.size(),
            cached_scan_results.data(), &num_results);
    CHECK(num_results >= 0 && static_cast<uint32_t>(num_results) <= kMaxCachedGscanResults);
    cached_scan_results.resize(num_results);
    // Check for invalid IE lengths in these cached scan results and correct it.
    for (auto& cached_scan_result : cached_scan_results) {
        int num_scan_results = cached_scan_result.num_results;
        for (int i = 0; i < num_scan_results; i++) {
            auto& scan_result = cached_scan_result.results[i];
            if (scan_result.ie_length > 0) {
                LOG(DEBUG) << "Cached scan result has non-zero IE length " << scan_result.ie_length;
                scan_result.ie_length = 0;
            }
        }
    }
    return {status, std::move(cached_scan_results)};
}

wifi_error WifiLegacyHal::createVirtualInterface(const std::string& ifname,
                                                 wifi_interface_type iftype) {
    // Create the interface if it doesn't exist. If interface already exist,
    // Vendor Hal should return WIFI_SUCCESS.
    wifi_error status = global_func_table_.wifi_virtual_interface_create(global_handle_,
                                                                         ifname.c_str(), iftype);
    return handleVirtualInterfaceCreateOrDeleteStatus(ifname, status);
}

wifi_error WifiLegacyHal::deleteVirtualInterface(const std::string& ifname) {
    // Delete the interface if it was created dynamically.
    wifi_error status =
            global_func_table_.wifi_virtual_interface_delete(global_handle_, ifname.c_str());
    return handleVirtualInterfaceCreateOrDeleteStatus(ifname, status);
}

wifi_error WifiLegacyHal::handleVirtualInterfaceCreateOrDeleteStatus(const std::string& ifname,
                                                                     wifi_error status) {
    if (status == WIFI_SUCCESS) {
        // refresh list of handlers now.
        status = retrieveIfaceHandles();
    } else if (status == WIFI_ERROR_NOT_SUPPORTED) {
        // Vendor hal does not implement this API. Such vendor implementations
        // are expected to create / delete interface by other means.

        // check if interface exists.
        if (if_nametoindex(ifname.c_str())) {
            status = retrieveIfaceHandles();
        }
    }
    return status;
}

wifi_error WifiLegacyHal::getSupportedIfaceName(uint32_t iface_type, std::string& ifname) {
    std::array<char, IFNAMSIZ> buffer;

    wifi_error res = global_func_table_.wifi_get_supported_iface_name(
            global_handle_, (uint32_t)iface_type, buffer.data(), buffer.size());
    if (res == WIFI_SUCCESS) ifname = buffer.data();

    return res;
}

wifi_error WifiLegacyHal::multiStaSetPrimaryConnection(const std::string& ifname) {
    return global_func_table_.wifi_multi_sta_set_primary_connection(global_handle_,
                                                                    getIfaceHandle(ifname));
}

wifi_error WifiLegacyHal::multiStaSetUseCase(wifi_multi_sta_use_case use_case) {
    return global_func_table_.wifi_multi_sta_set_use_case(global_handle_, use_case);
}

wifi_error WifiLegacyHal::setCoexUnsafeChannels(
        std::vector<wifi_coex_unsafe_channel> unsafe_channels, uint32_t restrictions) {
    return global_func_table_.wifi_set_coex_unsafe_channels(global_handle_, unsafe_channels.size(),
                                                            unsafe_channels.data(), restrictions);
}

wifi_error WifiLegacyHal::setVoipMode(const std::string& iface_name, wifi_voip_mode mode) {
    return global_func_table_.wifi_set_voip_mode(getIfaceHandle(iface_name), mode);
}

std::pair<wifi_twt_capabilities, wifi_error> WifiLegacyHal::twtGetCapabilities(
        const std::string& ifaceName) {
    wifi_twt_capabilities capabs = {};
    wifi_error status =
            global_func_table_.wifi_twt_get_capabilities(getIfaceHandle(ifaceName), &capabs);
    return {capabs, status};
}

void invalidateTwtInternalCallbacks() {
    on_twt_failure_internal_callback = nullptr;
    on_twt_session_create_internal_callback = nullptr;
    on_twt_session_update_internal_callback = nullptr;
    on_twt_session_teardown_internal_callback = nullptr;
    on_twt_session_stats_internal_callback = nullptr;
    on_twt_session_suspend_internal_callback = nullptr;
    on_twt_session_resume_internal_callback = nullptr;
}

wifi_error WifiLegacyHal::twtRegisterEvents(
        const std::string& ifaceName, const on_twt_failure& on_twt_failure_user_callback,
        const on_twt_session_create& on_twt_session_create_user_callback,
        const on_twt_session_update& on_twt_session_update_user_callback,
        const on_twt_session_teardown& on_twt_session_teardown_user_callback,
        const on_twt_session_stats& on_twt_session_stats_user_callback,
        const on_twt_session_suspend& on_twt_session_suspend_user_callback,
        const on_twt_session_resume& on_twt_session_resume_user_callback) {
    if (on_twt_failure_internal_callback || on_twt_session_create_internal_callback ||
        on_twt_session_update_internal_callback || on_twt_session_teardown_internal_callback ||
        on_twt_session_stats_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    on_twt_failure_internal_callback = [on_twt_failure_user_callback](
                                               wifi_request_id id, wifi_twt_error_code error_code) {
        on_twt_failure_user_callback(id, error_code);
    };

    on_twt_session_create_internal_callback = [on_twt_session_create_user_callback](
                                                      wifi_request_id id,
                                                      wifi_twt_session twt_session) {
        on_twt_session_create_user_callback(id, twt_session);
    };

    on_twt_session_update_internal_callback = [on_twt_session_update_user_callback](
                                                      wifi_request_id id,
                                                      wifi_twt_session twt_session) {
        on_twt_session_update_user_callback(id, twt_session);
    };

    on_twt_session_teardown_internal_callback = [on_twt_session_teardown_user_callback](
                                                        wifi_request_id id, int session_id,
                                                        wifi_twt_teardown_reason_code reason_code) {
        on_twt_session_teardown_user_callback(id, session_id, reason_code);
    };

    on_twt_session_stats_internal_callback = [on_twt_session_stats_user_callback](
                                                     wifi_request_id id, int session_id,
                                                     wifi_twt_session_stats stats) {
        on_twt_session_stats_user_callback(id, session_id, stats);
    };

    on_twt_session_suspend_internal_callback = [on_twt_session_suspend_user_callback](
                                                       wifi_request_id id, int session_id) {
        on_twt_session_suspend_user_callback(id, session_id);
    };

    on_twt_session_resume_internal_callback = [on_twt_session_resume_user_callback](
                                                      wifi_request_id id, int session_id) {
        on_twt_session_resume_user_callback(id, session_id);
    };

    wifi_error status = global_func_table_.wifi_twt_register_events(
            getIfaceHandle(ifaceName),
            {onAsyncTwtError, onAsyncTwtSessionCreate, onAsyncTwtSessionUpdate,
             onAsyncTwtSessionTeardown, onAsyncTwtSessionStats, onAsyncTwtSessionSuspend,
             onAsyncTwtSessionResume});
    if (status != WIFI_SUCCESS) {
        invalidateTwtInternalCallbacks();
    }
    return status;
}

wifi_error WifiLegacyHal::twtSessionSetup(const std::string& ifaceName, uint32_t cmdId,
                                          const wifi_twt_request& request) {
    return global_func_table_.wifi_twt_session_setup(cmdId, getIfaceHandle(ifaceName), request);
}

wifi_error WifiLegacyHal::twtSessionUpdate(const std::string& ifaceName, uint32_t cmdId,
                                           uint32_t sessionId, const wifi_twt_request& request) {
    return global_func_table_.wifi_twt_session_update(cmdId, getIfaceHandle(ifaceName), sessionId,
                                                      request);
}

wifi_error WifiLegacyHal::twtSessionSuspend(const std::string& ifaceName, uint32_t cmdId,
                                            uint32_t sessionId) {
    return global_func_table_.wifi_twt_session_suspend(cmdId, getIfaceHandle(ifaceName), sessionId);
}

wifi_error WifiLegacyHal::twtSessionResume(const std::string& ifaceName, uint32_t cmdId,
                                           uint32_t sessionId) {
    return global_func_table_.wifi_twt_session_resume(cmdId, getIfaceHandle(ifaceName), sessionId);
}

wifi_error WifiLegacyHal::twtSessionTeardown(const std::string& ifaceName, uint32_t cmdId,
                                             uint32_t sessionId) {
    return global_func_table_.wifi_twt_session_teardown(cmdId, getIfaceHandle(ifaceName),
                                                        sessionId);
}

wifi_error WifiLegacyHal::twtSessionGetStats(const std::string& ifaceName, uint32_t cmdId,
                                             uint32_t sessionId) {
    return global_func_table_.wifi_twt_session_get_stats(cmdId, getIfaceHandle(ifaceName),
                                                         sessionId);
}

wifi_error WifiLegacyHal::twtRegisterHandler(const std::string& iface_name,
                                             const TwtCallbackHandlers& user_callbacks) {
    on_twt_event_setup_response_callback = user_callbacks.on_setup_response;
    on_twt_event_teardown_completion_callback = user_callbacks.on_teardown_completion;
    on_twt_event_info_frame_received_callback = user_callbacks.on_info_frame_received;
    on_twt_event_device_notify_callback = user_callbacks.on_device_notify;

    return global_func_table_.wifi_twt_register_handler(
            getIfaceHandle(iface_name),
            {onAsyncTwtEventSetupResponse, onAsyncTwtEventTeardownCompletion,
             onAsyncTwtEventInfoFrameReceived, onAsyncTwtEventDeviceNotify});
}

std::pair<wifi_error, TwtCapabilitySet> WifiLegacyHal::twtGetCapability(
        const std::string& iface_name) {
    TwtCapabilitySet capSet;
    wifi_error status =
            global_func_table_.wifi_twt_get_capability(getIfaceHandle(iface_name), &capSet);
    return {status, capSet};
}

wifi_error WifiLegacyHal::twtSetupRequest(const std::string& iface_name,
                                          const TwtSetupRequest& msg) {
    TwtSetupRequest msgInternal(msg);
    return global_func_table_.wifi_twt_setup_request(getIfaceHandle(iface_name), &msgInternal);
}

wifi_error WifiLegacyHal::twtTearDownRequest(const std::string& iface_name,
                                             const TwtTeardownRequest& msg) {
    TwtTeardownRequest msgInternal(msg);
    return global_func_table_.wifi_twt_teardown_request(getIfaceHandle(iface_name), &msgInternal);
}

wifi_error WifiLegacyHal::twtInfoFrameRequest(const std::string& iface_name,
                                              const TwtInfoFrameRequest& msg) {
    TwtInfoFrameRequest msgInternal(msg);
    return global_func_table_.wifi_twt_info_frame_request(getIfaceHandle(iface_name), &msgInternal);
}

std::pair<wifi_error, TwtStats> WifiLegacyHal::twtGetStats(const std::string& iface_name,
                                                           uint8_t configId) {
    TwtStats stats;
    wifi_error status =
            global_func_table_.wifi_twt_get_stats(getIfaceHandle(iface_name), configId, &stats);
    return {status, stats};
}

wifi_error WifiLegacyHal::twtClearStats(const std::string& iface_name, uint8_t configId) {
    return global_func_table_.wifi_twt_clear_stats(getIfaceHandle(iface_name), configId);
}

wifi_error WifiLegacyHal::setScanMode(const std::string& iface_name, bool enable) {
    return global_func_table_.wifi_set_scan_mode(getIfaceHandle(iface_name), enable);
}

wifi_error WifiLegacyHal::setDtimConfig(const std::string& iface_name, uint32_t multiplier) {
    return global_func_table_.wifi_set_dtim_config(getIfaceHandle(iface_name), multiplier);
}

std::pair<wifi_error, std::vector<wifi_usable_channel>> WifiLegacyHal::getUsableChannels(
        uint32_t band_mask, uint32_t iface_mode_mask, uint32_t filter_mask) {
    std::vector<wifi_usable_channel> channels;
    channels.resize(kMaxWifiUsableChannels);
    uint32_t size = 0;
    wifi_error status = global_func_table_.wifi_get_usable_channels(
            global_handle_, band_mask, iface_mode_mask, filter_mask, channels.size(), &size,
            reinterpret_cast<wifi_usable_channel*>(channels.data()));
    CHECK(size >= 0 && size <= kMaxWifiUsableChannels);
    channels.resize(size);
    return {status, std::move(channels)};
}

wifi_error WifiLegacyHal::triggerSubsystemRestart() {
    return global_func_table_.wifi_trigger_subsystem_restart(global_handle_);
}

wifi_error WifiLegacyHal::setIndoorState(bool isIndoor) {
    return global_func_table_.wifi_set_indoor_state(global_handle_, isIndoor);
}

std::pair<wifi_error, wifi_radio_combination_matrix*>
WifiLegacyHal::getSupportedRadioCombinationsMatrix() {
    char* buffer = new char[kMaxSupportedRadioCombinationsMatrixLength];
    std::fill(buffer, buffer + kMaxSupportedRadioCombinationsMatrixLength, 0);
    uint32_t size = 0;
    wifi_radio_combination_matrix* radio_combination_matrix_ptr =
            reinterpret_cast<wifi_radio_combination_matrix*>(buffer);
    wifi_error status = global_func_table_.wifi_get_supported_radio_combinations_matrix(
            global_handle_, kMaxSupportedRadioCombinationsMatrixLength, &size,
            radio_combination_matrix_ptr);
    CHECK(size >= 0 && size <= kMaxSupportedRadioCombinationsMatrixLength);
    return {status, radio_combination_matrix_ptr};
}

wifi_error WifiLegacyHal::chreNanRttRequest(const std::string& iface_name, bool enable) {
    if (enable)
        return global_func_table_.wifi_nan_rtt_chre_enable_request(0, getIfaceHandle(iface_name),
                                                                   NULL);
    else
        return global_func_table_.wifi_nan_rtt_chre_disable_request(0, getIfaceHandle(iface_name));
}

wifi_error WifiLegacyHal::chreRegisterHandler(const std::string& iface_name,
                                              const ChreCallbackHandlers& handler) {
    if (on_chre_nan_rtt_internal_callback) {
        return WIFI_ERROR_NOT_AVAILABLE;
    }

    on_chre_nan_rtt_internal_callback = handler.on_wifi_chre_nan_rtt_state;

    wifi_error status = global_func_table_.wifi_chre_register_handler(getIfaceHandle(iface_name),
                                                                      {onAsyncChreNanRttState});
    if (status != WIFI_SUCCESS) {
        on_chre_nan_rtt_internal_callback = nullptr;
    }
    return status;
}

wifi_error WifiLegacyHal::enableWifiTxPowerLimits(const std::string& iface_name, bool enable) {
    return global_func_table_.wifi_enable_tx_power_limits(getIfaceHandle(iface_name), enable);
}

wifi_error WifiLegacyHal::getWifiCachedScanResults(const std::string& iface_name,
                                                   WifiCachedScanReport& report) {
    on_cached_scan_results_internal_callback = [&report](wifi_cached_scan_report* report_ptr) {
        report.results.assign(report_ptr->results, report_ptr->results + report_ptr->result_cnt);
        report.scanned_freqs.assign(report_ptr->scanned_freq_list,
                                    report_ptr->scanned_freq_list + report_ptr->scanned_freq_num);
        report.ts = report_ptr->ts;
    };
    wifi_error status = global_func_table_.wifi_get_cached_scan_results(getIfaceHandle(iface_name),
                                                                        {onSyncCachedScanResults});
    on_cached_scan_results_internal_callback = nullptr;
    return status;
}

std::pair<wifi_error, wifi_chip_capabilities> WifiLegacyHal::getWifiChipCapabilities() {
    wifi_chip_capabilities chip_capabilities;
    wifi_error status =
            global_func_table_.wifi_get_chip_capabilities(global_handle_, &chip_capabilities);
    return {status, chip_capabilities};
}

wifi_error WifiLegacyHal::enableStaChannelForPeerNetwork(uint32_t channelCategoryEnableFlag) {
    return global_func_table_.wifi_enable_sta_channel_for_peer_network(global_handle_,
                                                                       channelCategoryEnableFlag);
}

wifi_error WifiLegacyHal::setMloMode(wifi_mlo_mode mode) {
    return global_func_table_.wifi_set_mlo_mode(global_handle_, mode);
}

std::pair<wifi_error, wifi_iface_concurrency_matrix>
WifiLegacyHal::getSupportedIfaceConcurrencyMatrix() {
    wifi_iface_concurrency_matrix iface_concurrency_matrix;
    wifi_error status = global_func_table_.wifi_get_supported_iface_concurrency_matrix(
            global_handle_, &iface_concurrency_matrix);
    return {status, iface_concurrency_matrix};
}

void WifiLegacyHal::invalidate() {
    global_handle_ = nullptr;
    iface_name_to_handle_.clear();
    on_driver_memory_dump_internal_callback = nullptr;
    on_firmware_memory_dump_internal_callback = nullptr;
    on_gscan_event_internal_callback = nullptr;
    on_gscan_full_result_internal_callback = nullptr;
    on_link_layer_stats_result_internal_callback = nullptr;
    on_link_layer_ml_stats_result_internal_callback = nullptr;
    on_rssi_threshold_breached_internal_callback = nullptr;
    on_ring_buffer_data_internal_callback = nullptr;
    on_error_alert_internal_callback = nullptr;
    on_radio_mode_change_internal_callback = nullptr;
    on_subsystem_restart_internal_callback = nullptr;
    invalidateRttResultsCallbacks();
    on_nan_notify_response_user_callback = nullptr;
    on_nan_event_publish_terminated_user_callback = nullptr;
    on_nan_event_match_user_callback = nullptr;
    on_nan_event_match_expired_user_callback = nullptr;
    on_nan_event_subscribe_terminated_user_callback = nullptr;
    on_nan_event_followup_user_callback = nullptr;
    on_nan_event_disc_eng_event_user_callback = nullptr;
    on_nan_event_disabled_user_callback = nullptr;
    on_nan_event_tca_user_callback = nullptr;
    on_nan_event_beacon_sdf_payload_user_callback = nullptr;
    on_nan_event_data_path_request_user_callback = nullptr;
    on_nan_event_pairing_request_user_callback = nullptr;
    on_nan_event_pairing_confirm_user_callback = nullptr;
    on_nan_event_bootstrapping_request_user_callback = nullptr;
    on_nan_event_bootstrapping_confirm_user_callback = nullptr;
    on_nan_event_data_path_confirm_user_callback = nullptr;
    on_nan_event_data_path_end_user_callback = nullptr;
    on_nan_event_transmit_follow_up_user_callback = nullptr;
    on_nan_event_range_request_user_callback = nullptr;
    on_nan_event_range_report_user_callback = nullptr;
    on_nan_event_schedule_update_user_callback = nullptr;
    on_twt_event_setup_response_callback = nullptr;
    on_twt_event_teardown_completion_callback = nullptr;
    on_twt_event_info_frame_received_callback = nullptr;
    on_twt_event_device_notify_callback = nullptr;
    on_chre_nan_rtt_internal_callback = nullptr;
    on_cached_scan_results_internal_callback = nullptr;
    invalidateTwtInternalCallbacks();
}

}  // namespace legacy_hal
}  // namespace wifi
}  // namespace hardware
}  // namespace android
}  // namespace aidl
