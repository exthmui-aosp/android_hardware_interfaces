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

/**
 * Contains Beidou almanac data.
 * This is defined in BDS-SIS-ICD-B1I-3.0, section 5.2.4.15.
 *
 * @hide
 */
@VintfStability
parcelable BeidouAlmanac {
    /**
     * Contains Beidou satellite almanac data.
     * This is defined in BDS-SIS-ICD-B1I-3.0, section 5.2.4.15.
     */
    @VintfStability
    parcelable BeidouSatelliteAlmanac {
        /** The PRN number of the Beidou satellite. */
        int prn;

        /** Satellite health (0=healthy, 1=unhealthy). */
        int svHealth;

        /** Almanac reference time in seconds. */
        int toaSeconds;

        /** Eccentricity. */
        double eccentricity;

        /**
         * Correction of inclination angle relative to reference value at reference time
         * in semi-circles.
         */
        double deltaI;

        /** Argument of perigee in semi-circles. */
        double omega;

        /** Longitude of ascending node of orbital plane at weekly epoch in semi-circles. */
        double omega0;

        /** Rate of right ascension in semi-circles per second. */
        double omegaDot;

        /** Square root of semi-major axis in square root of meters. */
        double rootA;

        /** Mean anomaly at reference time in semi-circles. */
        double m0;

        /** Satellite clock time bias correction coefficient in seconds. */
        double af0;

        /** Satellite clock time drift correction coefficient in seconds per second. */
        double af1;
    }

    /** Beidou week number. */
    int beidouWeekNumber;

    /** Array of BeidouSatelliteAlmanac. */
    BeidouSatelliteAlmanac[] satelliteAlmanac;
}
