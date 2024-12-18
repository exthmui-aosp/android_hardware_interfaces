package android.hardware.gnss.gnss_assistance;

/**
 * Contains Glonass almanac data.
 * This is defined in Glonass ICD v5.1, section 4.5.
 *
 * @hide
 */
@VintfStability
parcelable GlonassAlmanac {
    /**
     * Contains Glonass satellite almanac data.
     * This is defined in Glonass ICD v5.1, section 4.5.
     */
    @VintfStability
    parcelable GlonassSatelliteAlmanac {
        /** Slot number. */
        int slotNumber;

        /**
         * Satellite health which is set with the GLONASS_HEALTH_STATUS_*
         * constants in GlonassSatelliteEphemeris.
         */
        int svHealth;

        /**
         * Frequency channel number.
         *
         * This is defined in Glonass ICD v5.1 section 3.3.1.1.
         */
        int frequencyChannelNumber;

        /** Calendar day number within the four-year period beginning since the leap year. */
        int calendarDayNumber;

        /** Flag to indicates if the satellite is a GLONASS-M satellitee. */
        boolean isGlonassM;

        /** Coarse value of satellite time correction to GLONASS time in seconds. */
        double tau;

        /** Time of first ascending node passage of satellite in seconds. */
        double tLambda;

        /** Longitude of the first ascending node in semi-circles. */
        double lambda;

        /** Correction to the mean value of inclination in semi-circles. */
        double deltaI;

        /** Correction to the mean value of the draconian period in seconds per orbital period. */
        double deltaT;

        /** Rate of change of draconian period in seconds per orbital period squared. */
        double deltaTDot;

        /** Eccentricity. */
        double eccentricity;

        /** Argument of perigee in semi-circles. */
        double omega;
    }

    /** Almanac issue date in milliseconds (UTC). */
    long issueDateMs;

    /** Array of GlonassSatelliteAlmanac. */
    GlonassSatelliteAlmanac[] satelliteAlmanacs;
}
