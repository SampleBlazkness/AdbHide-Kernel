package com.blazkness.adbng;

import java.io.ByteArrayOutputStream;

/** Thin wrapper around "su -c": every command in this app goes through it. */
final class Sh {

    static final String DIR = "/data/adb/adbng";
    static final String CTL = DIR + "/adbngctl.sh";

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
            r.out = "无法执行 su：" + t.getMessage();
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