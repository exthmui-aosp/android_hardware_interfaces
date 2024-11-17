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

package android.hardware.gnss.gnss_assistance;

/*
 * Contains the reference time of the GNSS clock.
 *
 * @hide
 */
@VintfStability
parcelable TimeOfClock {
    /** Year of the clock. */
    int year;

    /** Month of the clock. */
    int month;

    /** Day of the clock. */
    int day;

    /** Hour of the clock. */
    int hour;

    /** Minute of the clock. */
    int minute;

    /** Second of the clock. */
    int seconds;
}
