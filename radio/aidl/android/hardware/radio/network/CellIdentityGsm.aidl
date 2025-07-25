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

import android.hardware.radio.network.OperatorInfo;

/** @hide */
@VintfStability
@JavaDerive(toString=true)
@RustDerive(Clone=true, Eq=true, PartialEq=true)
parcelable CellIdentityGsm {
    /**
     * 3-digit Mobile Country Code, 0..999, empty string if unknown
     */
    String mcc;
    /**
     * 2 or 3-digit Mobile Network Code, 0..999, empty string if unknown
     */
    String mnc;
    /**
     * 16-bit Location Area Code, 0..65535, RadioConst:VALUE_UNAVAILABLE if unknown
     */
    int lac;
    /**
     * 16-bit GSM Cell Identity described in TS 27.007, 0..65535,
     * RadioConst:VALUE_UNAVAILABLE if unknown
     */
    int cid;
    /**
     * 16-bit GSM Absolute RF channel number; this value must be valid
     */
    int arfcn;
    /**
     * 6-bit Base Station Identity Code, RadioConst:VALUE_UNAVAILABLE_BYTE if unknown
     */
    byte bsic;
    /**
     * OperatorInfo containing alphaLong and alphaShort
     */
    OperatorInfo operatorNames;
    /**
     * Additional PLMN-IDs beyond the primary PLMN broadcast for this cell
     */
    String[] additionalPlmns;
}
