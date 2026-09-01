package com.ava.tool;

/** Host JVM test; kept outside java/ so it is never packaged into the APK. */
public final class AppearanceScheduleTest {
    private static void check(boolean expected, int mode, int minute, int light, int dark, boolean system) {
        boolean actual = AppearanceSchedule.isDark(mode, minute, light, dark, system);
        if (actual != expected) throw new AssertionError("mode=" + mode + " minute=" + minute + " expected dark=" + expected);
    }
    public static void main(String[] args) {
        for (int minute = 0; minute < 1440; ++minute) {
            check(minute < 420 || minute >= 1140, AppearanceSchedule.SCHEDULE, minute, 420, 1140, false);
            check(minute >= 300 && minute < 1200, AppearanceSchedule.SCHEDULE, minute, 1200, 300, true);
            check(false, AppearanceSchedule.LIGHT, minute, 420, 1140, true);
            check(true, AppearanceSchedule.DARK, minute, 420, 1140, false);
            check(true, AppearanceSchedule.SYSTEM, minute, 420, 1140, true);
            check(false, AppearanceSchedule.SYSTEM, minute, 420, 1140, false);
        }
        check(false, AppearanceSchedule.SCHEDULE, 420, 420, 1140, true);
        check(true, AppearanceSchedule.SCHEDULE, 1140, 420, 1140, false);
        check(true, AppearanceSchedule.SCHEDULE, -1, 420, 1140, false);
        check(false, AppearanceSchedule.SCHEDULE, 1860, 420, 1140, true);
        check(false, AppearanceSchedule.SCHEDULE, 600, 100, 100, true);
        check(false, AppearanceSchedule.SCHEDULE, 600, -1, 1800, true);
        check(true, 99, 600, 420, 1140, true);
        System.out.println("PASS 8647 appearance policy cases: system, manual, local schedule, boundaries and midnight wrapping");
    }
}
