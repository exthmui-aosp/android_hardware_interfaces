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

package android.hardware.radio.network;

/**
 * Type to define the LTE specific network capabilities for voice over PS including emergency and
 * normal voice calls.
 * @hide
 */
@VintfStability
@JavaDerive(toString=true)
@RustDerive(Clone=true, Eq=true, PartialEq=true)
parcelable LteVopsInfo {
    /**
     * This indicates if the camped network supports VoLTE services. This information is received from
     * LTE network during LTE NAS registration procedure through LTE ATTACH ACCEPT/TAU ACCEPT.
     * Refer 3GPP 24.301 EPS network feature support -> IMS VoPS
     */
    boolean isVopsSupported;
    /**
     * This indicates if the camped network supports VoLTE emergency bearers. This information is
     * received from LTE network through two sources:
     * a. During LTE NAS registration procedure through LTE ATTACH ACCEPT/TAU ACCEPT. Refer
     *    3GPP 24.301 EPS network feature support -> EMC BS
     * b. In case the device is not registered on network. Refer 3GPP 25.331 LTE RRC
     *    SIB1 : ims-EmergencySupport-r9
     * If the device is registered on LTE, then this field indicates (a).
     * In case of limited service on LTE this field indicates (b).
     */
    boolean isEmcBearerSupported;
}
