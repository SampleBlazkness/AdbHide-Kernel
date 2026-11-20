package com.blazkness.adbng;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Ships usbgwd + the scripts inside the APK and copies them to /data/adb/adbng
 * on first run, together with a service.d hook so the switch survives a reboot.
 */
final class Env {

    private static final String[] BLOBS = {"usbgwd", "adbngctl.sh", "usb-gadget.sh"};
    private static final String SERVICE = "/data/adb/service.d/95-adbng.sh";

    private static final String BOOT = "#!/system/bin/sh\n"
            + "# adbng boot restore - written by com.blazkness.adbng\n"
            + "[ -f /data/adb/adbng/enabled ] || exit 0\n"
            + "[ \"$(cat /data/adb/adbng/enabled)\" = \"1\" ] || exit 0\n"
            + "sleep 8\n"
            + "sh /data/adb/adbng/adbngctl.sh ensure >>/data/adb/adbng/boot.log 2>&1\n";

    /** True when the installed usbgwd is bit-identical in size to ours. */
    static boolean installed(Context ctx) {
        long mine = assetSize(ctx, "usbgwd");
        if (mine <= 0) {
            return false;
        }
        Sh.R r = Sh.run("stat -c %s " + Sh.DIR + "/usbgwd 2>/dev/null");
        return r.text().equals(String.valueOf(mine));
    }

    static long assetSize(Context ctx, String name) {
        try (InputStream in = ctx.getAssets().open(name)) {
            long total = 0;
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
            }
            return total;
        } catch (Exception e) {
            return -1;
        }
    }

    /** @return null on success, otherwise a message for the user. */
    static String install(Context ctx) {
        File stage = new File(ctx.getFilesDir(), "stage");
        //noinspection ResultOfMethodCallIgnored
        stage.mkdirs();
        try {
            for (String name : BLOBS) {
                copyAsset(ctx, name, new File(stage, name));
            }
            File boot = new File(ctx.getFilesDir(), "95-adbng.sh");
            try (OutputStream os = new FileOutputStream(boot)) {
                os.write(BOOT.getBytes("UTF-8"));
            }
        } catch (Exception e) {
            return "解包失败：" + e.getMessage();
        }

        String cmd = "mkdir -p " + Sh.DIR + " /data/adb/service.d"
                + " && cp -f " + stage.getAbsolutePath() + "/* " + Sh.DIR + "/"
                + " && cp -f " + ctx.getFilesDir() + "/95-adbng.sh " + SERVICE
                + " && chmod 755 " + Sh.DIR + "/usbgwd " + Sh.DIR + "/adbngctl.sh "
                + Sh.DIR + "/usb-gadget.sh " + SERVICE
                + " && ( [ -s " + Sh.DIR + "/adb_keys ] || : > " + Sh.DIR + "/adb_keys )"
                + " && chmod 600 " + Sh.DIR + "/adb_keys; ls -l " + Sh.DIR;
        Sh.R r = Sh.run(cmd);
        if (!r.ok()) {
            return "安装运行环境失败：" + r.text();
        }
        return installed(ctx) ? null : "安装后校验失败，请重试";
    }

    private static void copyAsset(Context ctx, String name, File dst) throws Exception {
        try (InputStream in = ctx.getAssets().open(name);
             OutputStream os = new FileOutputStream(dst)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
            }
        }
    }
}
