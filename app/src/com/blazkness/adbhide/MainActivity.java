package com.blazkness.adbhide;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.text.util.Linkify;
import java.util.regex.Pattern;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Status panel. Every value comes from the device itself: the kernel module's
 * /proc node, the module directories, the iptables chains and the uid list.
 * Reading them needs root, so the app has to be on the KernelSU superuser
 * list - KernelSU denies su for apps that are not listed, with no prompt.
 *
 * The panel states explicitly which protections are global and which apply
 * only to the uid blacklist, because that split decides what an unlisted app
 * (a new detector, for example) can still see.
 */
public class MainActivity extends Activity {

    private static final String SCRIPT =
            "p=/proc/adbhide_kmod\n" +
            "echo KMOD=$([ -r $p ] && echo 1 || echo 0)\n" +
            "if [ -r $p ]; then\n" +
            "  echo KVER=$(head -n1 $p | cut -d' ' -f2)\n" +
            "  echo KREDIR=$(sed -n 's/^redirect //p' $p)\n" +
            "  echo KNETSEQ=$(sed -n 's/^netseq //p' $p)\n" +
            "  echo KCOUNTERS=$(sed -n 's/^counters //p' $p)\n" +
            "  echo KTARGETS=$(sed -n 's/^targets [0-9]*://p' $p)\n" +
            "fi\n" +
            "echo NATIVE=$([ -f /data/adb/modules/adbhide_native/zygisk/arm64-v8a.so ] && echo 1 || echo 0)\n" +
            "echo NATIVEVER=$(sed -n 's/^version=//p' /data/adb/modules/adbhide_native/module.prop 2>/dev/null)\n" +
            "echo UIDS=$(tr '\\n' ' ' < /data/adb/adbhide/uids.txt 2>/dev/null)\n" +
            "echo CONFCOUNT=$(grep -v -e '^#' -e '^$' -e '=' /data/adb/adbhide/blacklist.conf | wc -l 2>/dev/null)\n" +
            "echo PORTS=$(iptables -S ADBHIDE_PORTS 2>/dev/null | grep -c uid-owner)\n" +
            "echo PORTS6=$(ip6tables -S ADBHIDE_PORTS 2>/dev/null | grep -c uid-owner)\n" +
            "echo MDNS=$(iptables -S ADBHIDE_MDNS_G 2>/dev/null | grep -c string)\n" +
            "echo MDNS6=$(ip6tables -S ADBHIDE_MDNS_G 2>/dev/null | grep -c string)\n" +
            "for u in $(cat /data/adb/adbhide/uids.txt 2>/dev/null); do\n" +
            "  n=$(pm list packages --uid $u 2>/dev/null | sed 's/package://; s/ uid:.*//' | head -n1)\n" +
            "  echo PKG_$u=$n\n" +
            "done\n";

    private TextView status;
    private TextView hint;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("AdbHide");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("内核 + Native + LSPosed");
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        subtitle.setTextColor(Color.GRAY);
        root.addView(subtitle);

        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        status.setTypeface(Typeface.MONOSPACE);
        status.setPadding(0, dp(16), 0, dp(12));
        status.setTextIsSelectable(true);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(status);
        root.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        Button sync = new Button(this);
        sync.setText("同步名单(包名 → uid)");
        sync.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                syncList();
            }
        });
        root.addView(sync);

        Button refresh = new Button(this);
        refresh.setText("刷新");
        refresh.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                refresh();
            }
        });
        root.addView(refresh);

        hint = new TextView(this);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        hint.setTextColor(Color.GRAY);
        hint.setPadding(0, dp(8), 0, 0);
        hint.setGravity(Gravity.START);
        root.addView(hint);

        setContentView(root);
        ensureConfig();
        refresh();
    }

    private void syncList() {
        hint.setText("正在同步…");
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Process p = Runtime.getRuntime().exec(new String[]{"su", "-c",
                            "sh /data/adb/modules/adbhide_kmod/sync.sh"});
                    p.waitFor();
                } catch (Throwable ignored) {
                }
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        refresh();
                    }
                });
            }
        }).start();
    }

    private static final String[] DEFAULT_PKGS = {
            "com.eltavine.duckdetector",
            "com.byxiaorun.detector",
            "com.chunqiunativecheck",
            "com.zhenxi.hunter",
            "luna.safe.luna",
            "com.aistudio.rootchecker.kxzqmr",
    };

    /** The app owns the list: create it with the defaults on first launch. */
    private void ensureConfig() {
        StringBuilder sb = new StringBuilder();
        sb.append("f=/data/adb/adbhide/blacklist.conf\n");
        sb.append("if [ ! -f $f ]; then\n");
        sb.append("  mkdir -p /data/adb/adbhide\n");
        sb.append("  printf '%s\n'");
        for (String p : DEFAULT_PKGS) {
            sb.append(" ").append(p);
        }
        sb.append(" > $f\n");
        sb.append("  chmod 600 $f\n");
        sb.append("fi\n");
        sb.append("sh /data/adb/modules/adbhide_kmod/sync.sh\n");
        try {
            Runtime.getRuntime().exec(new String[]{"su", "-c", sb.toString()}).waitFor();
        } catch (Throwable ignored) {
        }
    }

    private void refresh() {
        status.setText(readState());
        Linkify.addLinks(status, Pattern.compile("github\\.com/\\S+"), "https://");
    }

    private String readState() {
        Map<String, String> v = new HashMap<>();
        String error = null;

        Process proc = null;
        try {
            proc = Runtime.getRuntime().exec(new String[]{"su", "-c", SCRIPT});
            BufferedReader r = new BufferedReader(new InputStreamReader(proc.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) {
                    v.put(line.substring(0, eq), line.substring(eq + 1).trim());
                }
            }
            r.close();
            proc.waitFor();
        } catch (Throwable t) {
            error = t.toString();
        } finally {
            if (proc != null) {
                proc.destroy();
            }
        }

        boolean kmodOk = "1".equals(v.get("KMOD"));
        boolean nativeOk = "1".equals(v.get("NATIVE"));
        String uidsRaw = nz(v.get("UIDS")).trim();
        int uidCount = uidsRaw.isEmpty() ? 0 : uidsRaw.split("\\s+").length;
        int ports = num(v.get("PORTS"));
        int ports6 = num(v.get("PORTS6"));
        int mdns = num(v.get("MDNS")) + num(v.get("MDNS6"));
        boolean portsOk = ports >= uidCount * 2 && ports6 >= uidCount * 2;
        boolean mdnsOk = mdns >= 2;
        boolean readable = kmodOk || nativeOk || !uidsRaw.isEmpty()
                || ports + ports6 > 0;
        StringBuilder sb = new StringBuilder();

        sb.append("全局隐藏(系统属性)\n");
        sb.append("  ").append(mdnsOk ? "✅" : "❌").append(" 系统设置开关(adb/开发者/无线调试)\n");

        sb.append("\n");
        sb.append("应用隐藏(名单内 ").append(uidCount).append(" 个应用)\n");
        sb.append("  ").append(kmodOk ? "✅" : "❌").append(" 属性读取伪装\n");
        sb.append("  ").append(kmodOk ? "✅" : "❌").append(" 网络连接表\n");
        sb.append("  ").append(portsOk ? "✅" : "❌").append(" adbd 端口\n");
        sb.append("  ").append(nativeOk ? "✅" : "❌").append(" 进程内属性\n");

        sb.append("\n");
        sb.append("名单\n");
        sb.append("  /data/adb/adbhide/blacklist.conf\n");
        sb.append("  一行一个包名,改完点「同步名单」,不用重启\n");
        sb.append("  条目: ").append(nz(v.get("CONFCOUNT"))).append("\n");

        sb.append("\n");
        sb.append("名单明细\n");
        if (uidsRaw.isEmpty()) {
            sb.append("  (空)\n");
        } else {
            for (String u : uidsRaw.split("\\s+")) {
                String pkg = nz(v.get("PKG_" + u));
                if (pkg.isEmpty()) {
                    try {
                        String[] ps = getPackageManager().getPackagesForUid(Integer.parseInt(u));
                        if (ps != null && ps.length > 0) pkg = ps[0];
                    } catch (Throwable ignored) {
                    }
                }
                sb.append("  ").append(u).append("  ").append(pkg).append("\n");
            }
        }

        sb.append("\n");
        sb.append("子模块\n");
        sb.append("  ").append(kmodOk ? "✅ 内核 v" + nz(v.get("KVER")) : "❌ 内核模块未启用").append("\n");
        sb.append("  ").append(nativeOk ? "✅ Native " + nz(v.get("NATIVEVER")) : "❌ Native 模块未启用").append("\n");
        String selfVer = "";
        try {
            selfVer = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Throwable ignored) {
        }
        sb.append("  ✅ 应用 v").append(nz(selfVer)).append("\n");
        if (!kmodOk || !nativeOk) {
            sb.append("  未启用的请到 github.com/SampleBlazkness/AdbHide-Kernel/releases 下载\n");
        }

        sb.append("\n");
        sb.append("作者 Blazkness\n");

        if (!kmodOk && v.isEmpty()) {
            hint.setText("读不到模块状态:请在 KernelSU 管理器的「超级用户」里把 AdbHide 加入白名单"
                    + (error == null ? "" : "(" + error + ")"));
        } else {
            hint.setText("");
        }
        return sb.toString();
    }

    private static int num(String s) {
        try {
            return s == null ? 0 : Integer.parseInt(s.trim());
        } catch (Throwable t) {
            return 0;
        }
    }

    private static String nz(String s) {
        return s == null ? "" : s;
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                getResources().getDisplayMetrics());
    }
}
