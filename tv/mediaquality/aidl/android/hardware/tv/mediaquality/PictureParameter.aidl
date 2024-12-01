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

/**
 * The parameters for Picture Profile.
 */
@VintfStability
union PictureParameter {
    /*
     * Brightness field represents the brightness level of the TV.
     * Brightness value range are from 0.0 to 1.0, where 0.0 represents the minimum brightness and
     * 1.0 represents the maximum brightness. The content-unmodified value is 0.5.
     *
     * note: when a picture profile is applied to the entire display, the media quality framework
     * will synchronize the brightness field.
     */
    float brightness;

    /*
     * This value represents the image contrast on an arbitrary scale from 0 to 100,
     * where 0 represents the darkest black (black screen) and 100 represents the brightest
     * white (brighter).
     * The default/unmodified value for contrast is 50.
     */
    int contrast;

    /*
     * Control that increases edge contrast so that objects become more distinct.
     * Sharpness value range are from 0 to 100, where 0 represents the minimum sharpness that
     * makes the image appear softer with less defined edges, 100 represents the maximum
     * sharpness that makes the image appear halos around objects due to excessive edges.
     * The default/unmodified value for sharpness is 50.
     */
    int sharpness;

    /*
     * Saturation value controls the intensity or purity of colors.
     * Saturation values are from 0 to 100, where 0 represents grayscale (no color) and 100
     * represents the most vivid colors.
     * The default/unmodified value for saturation is 50.
     */
    int saturation;

    /*
     * Hue affects the balance between red, green and blue primary colors on the screen.
     * Hue values are from -50 to 50, where -50 represents cooler and 50 represents warmer.
     * The default/unmodified value for hue is 0.
     */
    int hue;
}
