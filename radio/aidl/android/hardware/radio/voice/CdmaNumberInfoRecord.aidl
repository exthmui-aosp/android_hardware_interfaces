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

package android.hardware.radio.voice;

/**
 * Called Party Number Info Rec as defined in C.S0005 section 3.7.5.2
 * Calling Party Number Info Rec as defined in C.S0005 section 3.7.5.3
 * Connected Number Info Rec as defined in C.S0005 section 3.7.5.4
 * @hide
 */
@VintfStability
@JavaDerive(toString=true)
@SuppressWarnings(value={"redundant-name"})
@RustDerive(Clone=true, Eq=true, PartialEq=true)
parcelable CdmaNumberInfoRecord {
    /** @deprecated Legacy CDMA is unsupported. */
    const int CDMA_NUMBER_INFO_BUFFER_LENGTH = 81;
    /**
     * Max length = CDMA_NUMBER_INFO_BUFFER_LENGTH
     * @deprecated Legacy CDMA is unsupported.
     */
    String number;
    /** @deprecated Legacy CDMA is unsupported. */
    byte numberType;
    /** @deprecated Legacy CDMA is unsupported. */
    byte numberPlan;
    /** @deprecated Legacy CDMA is unsupported. */
    byte pi;
    /** @deprecated Legacy CDMA is unsupported. */
    byte si;
}
