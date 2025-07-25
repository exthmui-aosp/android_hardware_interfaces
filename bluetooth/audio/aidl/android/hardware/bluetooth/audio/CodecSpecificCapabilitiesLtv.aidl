/*
 * Copyright 2023 The Android Open Source Project
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

package android.hardware.bluetooth.audio;

/**
 * Used to exchange generic remote device codec specific capabilities between
 * the stack and the provider. As defined in Bluetooth Assigned Numbers,
 * Sec. 6.12.4.
 */
@VintfStability
union CodecSpecificCapabilitiesLtv {
    /**
     * Supported sampling frequencies in Hertz
     */
    parcelable SupportedSamplingFrequencies {
        const int HZ8000 = 0x0001;
        const int HZ11025 = 0x0002;
        const int HZ16000 = 0x0004;
        const int HZ22050 = 0x0008;
        const int HZ24000 = 0x0010;
        const int HZ32000 = 0x0020;
        const int HZ44100 = 0x0040;
        const int HZ48000 = 0x0080;
        const int HZ88200 = 0x0100;
        const int HZ96000 = 0x0200;
        const int HZ176400 = 0x0400;
        const int HZ192000 = 0x0800;
        const int HZ384000 = 0x1000;

        /* 16 bits wide bit mask */
        int bitmask;
    }
    /**
     * Supported frame durations in microseconds
     */
    parcelable SupportedFrameDurations {
        const int US7500 = 0x01;
        const int US10000 = 0x02;
        const int US20000 = 0x04;
        /* Bits 2-3 are RFU */
        const int US7500PREFERRED = 0x10;
        const int US10000PREFERRED = 0x20;
        const int US20000PREFERRED = 0x40;

        /* 8 bit wide bit mask */
        int bitmask;
    }
    parcelable SupportedAudioChannelCounts {
        const int ONE = 0x01;
        const int TWO = 0x02;
        const int THREE = 0x04;
        const int FOUR = 0x08;
        const int FIVE = 0x10;
        const int SIX = 0x20;
        const int SEVEN = 0x40;
        const int EIGHT = 0x80;

        /* 8 bit wide bit mask */
        int bitmask;
    }
    parcelable SupportedOctetsPerCodecFrame {
        int min;
        int max;
    }
    parcelable SupportedMaxCodecFramesPerSDU {
        int value;
    }

    SupportedSamplingFrequencies supportedSamplingFrequencies;
    SupportedFrameDurations supportedFrameDurations;
    SupportedAudioChannelCounts supportedAudioChannelCounts;
    SupportedOctetsPerCodecFrame supportedOctetsPerCodecFrame;
    SupportedMaxCodecFramesPerSDU supportedMaxCodecFramesPerSDU;
}
