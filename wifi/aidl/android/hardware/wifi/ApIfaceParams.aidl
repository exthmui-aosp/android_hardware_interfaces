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

package android.hardware.wifi;

import android.hardware.wifi.IfaceConcurrencyType;
import android.hardware.wifi.common.OuiKeyedData;

/**
 * Parameters to use for setting up the access point interfaces.
 */
@VintfStability
parcelable ApIfaceParams {
    /**
     * IfaceConcurrencyType to be created. Takes one of
     * |IfaceConcurrencyType.AP| or |IfaceConcurrencyType.AP_BRIDGED|
     */
    IfaceConcurrencyType ifaceType;
    /**
     * Whether the current iface will be operated on Multi-links on the one MLD device (MLO).
     */
    boolean usesMlo;
    /**
     * Optional vendor-specific configuration parameters.
     */
    @nullable OuiKeyedData[] vendorData;
}
