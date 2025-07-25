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

#include "wifi_legacy_hal_stubs.h"

// TODO: Remove these stubs from HalTool in libwifi-system.
namespace aidl {
namespace android {
namespace hardware {
namespace wifi {
namespace legacy_hal {
template <typename>
struct stubFunction;

template <typename R, typename... Args>
struct stubFunction<R (*)(Args...)> {
    static constexpr R invoke(Args...) { return WIFI_ERROR_NOT_SUPPORTED; }
};
template <typename... Args>
struct stubFunction<void (*)(Args...)> {
    static constexpr void invoke(Args...) {}
};

template <typename T>
void populateStubFor(T* val) {
    *val = &stubFunction<T>::invoke;
}

bool initHalFuncTableWithStubs(wifi_hal_fn* hal_fn) {
    if (hal_fn == nullptr) {
        return false;
    }
    populateStubFor(&hal_fn->wifi_initialize);
    populateStubFor(&hal_fn->wifi_wait_for_driver_ready);
    populateStubFor(&hal_fn->wifi_cleanup);
    populateStubFor(&hal_fn->wifi_event_loop);
    populateStubFor(&hal_fn->wifi_get_error_info);
    populateStubFor(&hal_fn->wifi_get_supported_feature_set);
    populateStubFor(&hal_fn->wifi_get_concurrency_matrix);
    populateStubFor(&hal_fn->wifi_set_scanning_mac_oui);
    populateStubFor(&hal_fn->wifi_get_supported_channels);
    populateStubFor(&hal_fn->wifi_is_epr_supported);
    populateStubFor(&hal_fn->wifi_get_ifaces);
    populateStubFor(&hal_fn->wifi_get_iface_name);
    populateStubFor(&hal_fn->wifi_set_iface_event_handler);
    populateStubFor(&hal_fn->wifi_reset_iface_event_handler);
    populateStubFor(&hal_fn->wifi_start_gscan);
    populateStubFor(&hal_fn->wifi_stop_gscan);
    populateStubFor(&hal_fn->wifi_get_cached_gscan_results);
    populateStubFor(&hal_fn->wifi_set_bssid_hotlist);
    populateStubFor(&hal_fn->wifi_reset_bssid_hotlist);
    populateStubFor(&hal_fn->wifi_set_significant_change_handler);
    populateStubFor(&hal_fn->wifi_reset_significant_change_handler);
    populateStubFor(&hal_fn->wifi_get_gscan_capabilities);
    populateStubFor(&hal_fn->wifi_set_link_stats);
    populateStubFor(&hal_fn->wifi_get_link_stats);
    populateStubFor(&hal_fn->wifi_clear_link_stats);
    populateStubFor(&hal_fn->wifi_get_valid_channels);
    populateStubFor(&hal_fn->wifi_rtt_range_request);
    populateStubFor(&hal_fn->wifi_rtt_range_cancel);
    populateStubFor(&hal_fn->wifi_get_rtt_capabilities);
    populateStubFor(&hal_fn->wifi_rtt_get_responder_info);
    populateStubFor(&hal_fn->wifi_enable_responder);
    populateStubFor(&hal_fn->wifi_disable_responder);
    populateStubFor(&hal_fn->wifi_set_nodfs_flag);
    populateStubFor(&hal_fn->wifi_start_logging);
    populateStubFor(&hal_fn->wifi_set_epno_list);
    populateStubFor(&hal_fn->wifi_reset_epno_list);
    populateStubFor(&hal_fn->wifi_set_country_code);
    populateStubFor(&hal_fn->wifi_get_firmware_memory_dump);
    populateStubFor(&hal_fn->wifi_set_log_handler);
    populateStubFor(&hal_fn->wifi_reset_log_handler);
    populateStubFor(&hal_fn->wifi_set_alert_handler);
    populateStubFor(&hal_fn->wifi_reset_alert_handler);
    populateStubFor(&hal_fn->wifi_get_firmware_version);
    populateStubFor(&hal_fn->wifi_get_ring_buffers_status);
    populateStubFor(&hal_fn->wifi_get_logger_supported_feature_set);
    populateStubFor(&hal_fn->wifi_get_ring_data);
    populateStubFor(&hal_fn->wifi_enable_tdls);
    populateStubFor(&hal_fn->wifi_disable_tdls);
    populateStubFor(&hal_fn->wifi_get_tdls_status);
    populateStubFor(&hal_fn->wifi_get_tdls_capabilities);
    populateStubFor(&hal_fn->wifi_get_driver_version);
    populateStubFor(&hal_fn->wifi_set_passpoint_list);
    populateStubFor(&hal_fn->wifi_reset_passpoint_list);
    populateStubFor(&hal_fn->wifi_set_lci);
    populateStubFor(&hal_fn->wifi_set_lcr);
    populateStubFor(&hal_fn->wifi_start_sending_offloaded_packet);
    populateStubFor(&hal_fn->wifi_stop_sending_offloaded_packet);
    populateStubFor(&hal_fn->wifi_start_rssi_monitoring);
    populateStubFor(&hal_fn->wifi_stop_rssi_monitoring);
    populateStubFor(&hal_fn->wifi_get_wake_reason_stats);
    populateStubFor(&hal_fn->wifi_configure_nd_offload);
    populateStubFor(&hal_fn->wifi_get_driver_memory_dump);
    populateStubFor(&hal_fn->wifi_start_pkt_fate_monitoring);
    populateStubFor(&hal_fn->wifi_get_tx_pkt_fates);
    populateStubFor(&hal_fn->wifi_get_rx_pkt_fates);
    populateStubFor(&hal_fn->wifi_nan_enable_request);
    populateStubFor(&hal_fn->wifi_nan_disable_request);
    populateStubFor(&hal_fn->wifi_nan_publish_request);
    populateStubFor(&hal_fn->wifi_nan_publish_cancel_request);
    populateStubFor(&hal_fn->wifi_nan_subscribe_request);
    populateStubFor(&hal_fn->wifi_nan_subscribe_cancel_request);
    populateStubFor(&hal_fn->wifi_nan_transmit_followup_request);
    populateStubFor(&hal_fn->wifi_nan_stats_request);
    populateStubFor(&hal_fn->wifi_nan_config_request);
    populateStubFor(&hal_fn->wifi_nan_tca_request);
    populateStubFor(&hal_fn->wifi_nan_beacon_sdf_payload_request);
    populateStubFor(&hal_fn->wifi_nan_register_handler);
    populateStubFor(&hal_fn->wifi_nan_get_version);
    populateStubFor(&hal_fn->wifi_nan_get_capabilities);
    populateStubFor(&hal_fn->wifi_nan_data_interface_create);
    populateStubFor(&hal_fn->wifi_nan_data_interface_delete);
    populateStubFor(&hal_fn->wifi_nan_data_request_initiator);
    populateStubFor(&hal_fn->wifi_nan_data_indication_response);
    populateStubFor(&hal_fn->wifi_nan_pairing_request);
    populateStubFor(&hal_fn->wifi_nan_pairing_indication_response);
    populateStubFor(&hal_fn->wifi_nan_bootstrapping_request);
    populateStubFor(&hal_fn->wifi_nan_bootstrapping_indication_response);
    populateStubFor(&hal_fn->wifi_nan_data_end);
    populateStubFor(&hal_fn->wifi_nan_pairing_end);
    populateStubFor(&hal_fn->wifi_get_packet_filter_capabilities);
    populateStubFor(&hal_fn->wifi_set_packet_filter);
    populateStubFor(&hal_fn->wifi_read_packet_filter);
    populateStubFor(&hal_fn->wifi_get_roaming_capabilities);
    populateStubFor(&hal_fn->wifi_enable_firmware_roaming);
    populateStubFor(&hal_fn->wifi_configure_roaming);
    populateStubFor(&hal_fn->wifi_select_tx_power_scenario);
    populateStubFor(&hal_fn->wifi_reset_tx_power_scenario);
    populateStubFor(&hal_fn->wifi_set_radio_mode_change_handler);
    populateStubFor(&hal_fn->wifi_set_latency_mode);
    populateStubFor(&hal_fn->wifi_set_thermal_mitigation_mode);
    populateStubFor(&hal_fn->wifi_virtual_interface_create);
    populateStubFor(&hal_fn->wifi_virtual_interface_delete);
    populateStubFor(&hal_fn->wifi_map_dscp_access_category);
    populateStubFor(&hal_fn->wifi_reset_dscp_mapping);
    populateStubFor(&hal_fn->wifi_set_subsystem_restart_handler);
    populateStubFor(&hal_fn->wifi_get_supported_iface_name);
    populateStubFor(&hal_fn->wifi_early_initialize);
    populateStubFor(&hal_fn->wifi_get_chip_feature_set);
    populateStubFor(&hal_fn->wifi_multi_sta_set_primary_connection);
    populateStubFor(&hal_fn->wifi_multi_sta_set_use_case);
    populateStubFor(&hal_fn->wifi_set_coex_unsafe_channels);
    populateStubFor(&hal_fn->wifi_set_voip_mode);
    populateStubFor(&hal_fn->wifi_twt_register_handler);
    populateStubFor(&hal_fn->wifi_twt_get_capability);
    populateStubFor(&hal_fn->wifi_twt_setup_request);
    populateStubFor(&hal_fn->wifi_twt_teardown_request);
    populateStubFor(&hal_fn->wifi_twt_info_frame_request);
    populateStubFor(&hal_fn->wifi_twt_get_stats);
    populateStubFor(&hal_fn->wifi_twt_clear_stats);
    populateStubFor(&hal_fn->wifi_set_dtim_config);
    populateStubFor(&hal_fn->wifi_get_usable_channels);
    populateStubFor(&hal_fn->wifi_trigger_subsystem_restart);
    populateStubFor(&hal_fn->wifi_set_indoor_state);
    populateStubFor(&hal_fn->wifi_get_supported_radio_combinations_matrix);
    populateStubFor(&hal_fn->wifi_nan_rtt_chre_enable_request);
    populateStubFor(&hal_fn->wifi_nan_rtt_chre_disable_request);
    populateStubFor(&hal_fn->wifi_chre_register_handler);
    populateStubFor(&hal_fn->wifi_enable_tx_power_limits);
    populateStubFor(&hal_fn->wifi_get_cached_scan_results);
    populateStubFor(&hal_fn->wifi_get_chip_capabilities);
    populateStubFor(&hal_fn->wifi_enable_sta_channel_for_peer_network);
    populateStubFor(&hal_fn->wifi_nan_suspend_request);
    populateStubFor(&hal_fn->wifi_nan_resume_request);
    populateStubFor(&hal_fn->wifi_set_scan_mode);
    populateStubFor(&hal_fn->wifi_set_mlo_mode);
    populateStubFor(&hal_fn->wifi_get_supported_iface_concurrency_matrix);
    populateStubFor(&hal_fn->wifi_get_rtt_capabilities_v3);
    populateStubFor(&hal_fn->wifi_get_rtt_capabilities_v4);
    populateStubFor(&hal_fn->wifi_rtt_range_request_v3);
    populateStubFor(&hal_fn->wifi_rtt_range_request_v4);
    populateStubFor(&hal_fn->wifi_twt_get_capabilities);
    populateStubFor(&hal_fn->wifi_twt_register_events);
    populateStubFor(&hal_fn->wifi_twt_session_setup);
    populateStubFor(&hal_fn->wifi_twt_session_update);
    populateStubFor(&hal_fn->wifi_twt_session_suspend);
    populateStubFor(&hal_fn->wifi_twt_session_resume);
    populateStubFor(&hal_fn->wifi_twt_session_teardown);
    populateStubFor(&hal_fn->wifi_twt_session_get_stats);
    populateStubFor(&hal_fn->wifi_virtual_interface_create_with_vendor_data);
    return true;
}

}  // namespace legacy_hal
}  // namespace wifi
}  // namespace hardware
}  // namespace android
}  // namespace aidl
