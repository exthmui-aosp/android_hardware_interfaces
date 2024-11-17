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
 * Contains GPS almanac data.
 * This is defined in IS-GPS-200, section 20.3.3.5.1.2.
 *
 * @hide
 */
@VintfStability
parcelable GpsAlmanac {
    /** GPS week number. */
    int gpsWeekNumber;

    /** GPS time of week in seconds. */
    int secondsOfGpsWeek;

    /**
     * Contains GPS satellite almanac data.
     * This is defined in IS-GPS-200, section 20.3.3.5.1.2.
     */
    @VintfStability
    parcelable GpsSatelliteAlmanac {
        /** The PRN number of the GPS satellite. */
        int prn;

        /**
         * Satellite health information.
         * The satellite subframe 4 and 5, page 25 six-bit health code as defined
         * in IS-GPS-200 Table 20-VIII expressed in integer form.
         */
        int svHealth;

        /** Eccentricity. */
        double eccentricity;

        /**
         * Correction of inclination angle relative to reference value at
         * reference time in semi-circles.
         */
        double inclination;

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

    /** Array of GpsSatelliteAlmanac. */
    GpsSatelliteAlmanac[] satelliteAlmanac;
}
