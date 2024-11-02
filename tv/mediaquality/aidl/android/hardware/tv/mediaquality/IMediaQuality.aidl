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

package android.hardware.tv.mediaquality;

import android.hardware.tv.mediaquality.AmbientBacklightSettings;
import android.hardware.tv.mediaquality.IMediaQualityCallback;

/**
 * Interface for the media quality service
 */
@VintfStability
interface IMediaQuality {
    /**
     * Sets a callback for events.
     *
     * @param callback Callback object to pass events.
     */
    void setCallback(in IMediaQualityCallback callback);

    /**
     * Sets the ambient backlight detector settings.
     *
     * @param settings Ambient backlight detector settings.
     */
    void setAmbientBacklightDetector(in AmbientBacklightSettings settings);

    /**
     * Sets the ambient backlight detection enabled or disabled. The ambient backlight is the
     * projection of light against the wall driven by the current content playing. Enable will
     * detects the Ambient backlight metadata and ambient control app can control the related
     * device as configured before.
     *
     * @param enabled True to enable the ambient backlight detection, false to disable.
     */
    void setAmbientBacklightDetectionEnabled(in boolean enabled);

    /**
     * Gets the ambient backlight detection enabled status. The ambient backlight is enabled by
     * calling setAmbientBacklightDetectionEnabled(in boolean enabled). True to enable the
     * ambient light detection and False to disable the ambient backlight detection.
     *
     * @return True if the ambient backlight detection is enabled, false otherwise.
     */
    boolean getAmbientBacklightDetectionEnabled();
}
