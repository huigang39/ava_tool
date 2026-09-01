package com.ava.tool;

/** Local-clock appearance policy, independent of Activity and device I/O. */
public final class AppearanceSchedule {
    public static final int SYSTEM = 0, SCHEDULE = 1, LIGHT = 2, DARK = 3;
    private AppearanceSchedule() {}

    public static boolean isDark(int mode, int minuteOfDay, int lightStart, int darkStart, boolean systemDark) {
        if (mode == LIGHT) return false;
        if (mode == DARK) return true;
        if (mode != SCHEDULE) return systemDark;
        if (lightStart < 0 || lightStart >= 1440 || darkStart < 0 || darkStart >= 1440 || lightStart == darkStart) {
            lightStart = 7 * 60; darkStart = 19 * 60;
        }
        int now = Math.floorMod(minuteOfDay, 1440);
        boolean light = lightStart < darkStart ? now >= lightStart && now < darkStart : now >= lightStart || now < darkStart;
        return !light;
    }
}
