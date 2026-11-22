package com.blazkness.adbng;

import android.content.Context;

import java.io.ByteArrayOutputStream;

/** Thin wrapper around "su -c": every command in this app goes through it. */
final class Sh {

    static final String DIR = "/data/adb/adbng";
    static final String CTL = DIR + "/adbngctl.sh";

    /** Set once from the activity, only used to localise the failure message. */
    private static Context app;

    static final class R {
        int code = -1;
        String out = "";

        boolean ok() {
            return code == 0;
        }

        String text() {
            return out.trim();
        }
    }

    static void init(Context c) {
        app = c == null ? null : c.getApplicationContext();
    }

    private static String su(String detail) {
        return app == null
                ? "su: " + detail
                : app.getString(com.blazkness.adbng.R.string.adbng_su_failed, detail);
    }

    static R run(String cmd) {
        R r = new R();
        Process p = null;
        try {
            p = new ProcessBuilder("su", "-c", cmd).redirectErrorStream(true).start();
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = p.getInputStream().read(buf)) > 0) {
                bo.write(buf, 0, n);
            }
            r.code = p.waitFor();
            r.out = bo.toString("UTF-8");
        } catch (Throwable t) {
            r.code = -2;
            r.out = su(String.valueOf(t.getMessage()));
        } finally {
            if (p != null) {
                p.destroy();
            }
        }
        return r;
    }

    static R ctl(String args) {
        return run("sh " + CTL + " " + args);
    }
}