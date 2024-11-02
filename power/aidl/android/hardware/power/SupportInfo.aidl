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

package android.hardware.power;

import android.hardware.power.Boost;
import android.hardware.power.Mode;
import android.hardware.power.SessionHint;
import android.hardware.power.SessionMode;

/**
 * Tells clients the status of various PowerHAL features in a single call.
 * SupportInfo consists of several bitsets, where each bit from the left
 * corresponds to the support status of that same value of that enum index.
 *
 * For "Boost", having the first bit set would mean "INTERACTION"
 * boost is supported, having the second bit set would mean
 * "DISPLAY_UPDATE_IMMINENT" is supported, and so on. The expectation
 * is that a client should be able to index the bitset like
 * "(supportInfo.boosts >> Boost::AUDIO_LAUNCH) % 2" and it should return
 * the support value of Boost::AUDIO_LAUNCH. This pattern is the same for
 * all four support bitsets.
 */
@VintfStability
parcelable SupportInfo {
    /**
     * Boolean representing whether hint sessions are supported on this device
     */
    boolean usesSessions;

    /**
     * The set of "Boost" enum values that are supported by this device
     * Each bit should correspond to a value of the "Boost.aidl" enum
     */
    long boosts;

    /**
     * The set of "Mode" enum values that are supported by this device
     * Each bit should correspond to a value of the "Mode.aidl" enum
     */
    long modes;

    /**
     * The set of "SessionHint" enum values that are supported by this device
     * Each bit should correspond to a value of the "SessionHint.aidl" enum
     */
    long sessionHints;

    /**
     * The set of "SessionMode" enum values that are supported by this device
     * Each bit should correspond to a value of the "SessionMode.aidl" enum
     */
    long sessionModes;

    /**
     * The set of "SessionTag" enum values that are supported by this device
     * Each bit should correspond to a value of the "SessionTag.aidl" enum
     */
    long sessionTags;
}
