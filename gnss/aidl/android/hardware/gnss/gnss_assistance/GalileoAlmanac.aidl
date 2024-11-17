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
 * Contains Galileo almanac data.
 * This is defined in Galileo-OS-SIS-ICD-v2.1, 5.1.10.
 *
 * @hide
 */
@VintfStability
parcelable GalileoAlmanac {
    /**
     * Contains Galileo satellite almanac data.
     * This is defined in Galileo-OS-SIS-ICD-v2.1, 5.1.10.
     */
    @VintfStability
    parcelable GalileoSatelliteAlmanac {
        /** Satellite ID. */
        int svId;

        /** Satellite health status. */
        GalileoAlmanacSvHealth svHealth;

        /** Eccentricity. */
        double eccentricity;

        /**
         * Difference between the inclination angle at reference time and the
         * nominal inclination, in semi-circles.
         */
        double deltaI;

        /** Argument of perigee in semi-circles. */
        double omega;

        /** Longitude of ascending node of orbital plane at weekly epoch in semi-circles. */
        double omega0;

        /** Rate of right ascension in semi-circles per second. */
        double omegaDot;

        /**
         * Difference with respect to the square root of the nominal semi-major axis
         * in square root of meters.
         */
        double rootA;

        /** Satellite mean anomaly at reference time in semi-circles. */
        double m0;

        /** Satellite clock correction bias in seconds. */
        double af0;

        /** Satellite clock correction linear in seconds per second. */
        double af1;

        /**
         * Almanac reference week number.
         * Modulo 4 representation of the Galileo system time week number.
         */
        int weekNumber;

        /** Almanac reference time in seconds. */
        int toa;

        /** Almanac issue of data. */
        int iod;
    }

    /**
     * Contains Galileo satellite health status.
     */
    @VintfStability
    parcelable GalileoAlmanacSvHealth {
        /** Satellite E5a signal health status. */
        int fNavE5a;

        /** Satellite E5b signal health status. */
        int iNavE5b;

        /** Satellite E1b signal health status. */
        int iNavE1b;
    }

    /** Almanac reference UTC time in milliseconds */
    long issueDate;

    /**
     * Almanac reference week number.
     * Modulo 4 representation of the Galileo system time week number.
     */
    int weekNumber;

    /** Almanac reference time in seconds. */
    int toa;

    /** Almanac issue of data. */
    int iod;

    /** Array of GalileoSatelliteAlmanac. */
    GalileoSatelliteAlmanac[] satelliteAlmanac;
}
