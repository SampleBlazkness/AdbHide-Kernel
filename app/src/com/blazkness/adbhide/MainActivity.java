package com.blazkness.adbhide;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Environment;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
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
        refresh();
    }

    private void refresh() {
        status.setText(readState());
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
        boolean snapshot = false;
        String stamp = "";

        if (!readable) {
            Map<String, String> snap = readSnapshot();
            if (snap != null) {
                v = snap;
                snapshot = true;
                stamp = nz(v.get("STAMP"));
                kmodOk = "1".equals(v.get("KMOD"));
                nativeOk = "1".equals(v.get("NATIVE"));
                uidsRaw = nz(v.get("UIDS")).trim();
                uidCount = uidsRaw.isEmpty() ? 0 : uidsRaw.split("\\s+").length;
                ports = num(v.get("PORTS"));
                ports6 = num(v.get("PORTS6"));
                mdns = num(v.get("MDNS")) + num(v.get("MDNS6"));
                portsOk = ports >= uidCount * 2 && ports6 >= uidCount * 2;
                mdnsOk = mdns >= 2;
                readable = true;
            }
        }

        StringBuilder sb = new StringBuilder();
        if (snapshot) {
            sb.append("数据来自开机快照: ").append(stamp).append("\n");
            sb.append("KernelSU 未允许本应用执行 su,实时状态读不到;\n");
            sb.append("下方内容为最近一次开机时模块写入的状态。\n\n");
        }
        if (!readable) {
            sb.append("无法读取状态\n\n");
            sb.append("  当前进程没有 root,且没有可用的状态快照:\n");
            sb.append("  ").append(error == null ? "su 不可用" : error).append("\n\n");
            sb.append("  在 KernelSU 管理器里授予本应用 root 后再刷新。\n");
            hint.setText("读不到模块状态");
            return sb.toString();
        }

        sb.append("全局保护\n");
        sb.append("  ✅ Settings.Global 的 adb / 开发者 / 无线调试开关\n");
        sb.append("     对全部普通应用返回 0\n");
        sb.append("  ").append(mdnsOk ? "✅" : "❌")
                .append(" mDNS 不再广播 _adb 记录\n");
        sb.append("     整机生效,任何设备都发现不了这台手机\n");

        sb.append('\n');
        sb.append("按名单保护 (").append(uidCount).append(" 个 uid)\n");
        sb.append("  ").append(kmodOk ? "✅" : "❌")
                .append(" 属性伪装 ").append(nz(v.get("KREDIR"))).append('\n');
        sb.append("     persist.sys.usb.config / sys.usb.* / init.svc.adbd\n");
        sb.append("  ").append(kmodOk ? "✅" : "❌")
                .append(" /proc/net/tcp{,6} 隐藏 adbd 监听行 ").append(nz(v.get("KNETSEQ"))).append('\n');
        sb.append("  ").append(portsOk ? "✅" : "❌")
                .append(" adbd 端口对名单 uid 拒绝连接 (").append(ports)
                .append('/').append(ports6).append(" 条规则)\n");
        sb.append("  ").append(nativeOk ? "✅" : "❌")
                .append(" 名单进程内属性映射已私有化\n");
        sb.append("     覆盖 Java / JNI / 反射三种读法\n");

        sb.append('\n');
        sb.append("名单明细\n");
        String uids = nz(v.get("UIDS"));
        if (uids.isEmpty()) {
            sb.append("  (空)\n");
        } else {
            for (String u : uids.split("\\s+")) {
                if (u.isEmpty()) {
                    continue;
                }
                String pkg = nz(v.get("PKG_" + u));
                if (pkg.isEmpty()) {
                    try {
                        String[] ps = getPackageManager()
                                .getPackagesForUid(Integer.parseInt(u));
                        if (ps != null && ps.length > 0) {
                            pkg = ps[0];
                        }
                    } catch (Throwable ignored) {
                    }
                }
                sb.append("  ").append(u);
                if (!pkg.isEmpty()) {
                    sb.append("  ").append(pkg);
                }
                sb.append('\n');
            }
        }

        sb.append('\n');
        sb.append("子模块\n");
        sb.append("  ").append(kmodOk ? "✅" : "❌")
                .append(" adbhide_kmod ").append(nz(v.get("KVER"))).append('\n');
        sb.append("     ").append(nz(v.get("KCOUNTERS"))).append('\n');
        sb.append("  ").append(nativeOk ? "✅" : "❌")
                .append(" adbhide_native ").append(nz(v.get("NATIVEVER"))).append('\n');
        sb.append("  ").append(HookState.appHooked ? "✅" : "—")
                .append(" LSPosed 已注入本应用")
                .append(HookState.appHooked ? "\n" : "(不需要,Settings 层在系统框架里)\n");

        sb.append('\n');
        sb.append("不在保护范围\n");
        sb.append("  · root 本身:KernelSU、管理器、模块目录\n");
        sb.append("  · USB 设置界面里的开关(系统进程读真值)\n");
        sb.append("  · 名单外的应用可以看到全部真实状态\n");
        sb.append("  · 属性值是进程启动时的快照,之后不再跟随系统变化\n");
        sb.append("  · 自动发现失效:adb mdns / IDE 列表 / 扫码配对\n");
        sb.append("    手动 adb connect IP:5555、adb pair IP:端口 配对码 照常\n");

        if (!kmodOk && v.isEmpty()) {
            hint.setText("读不到模块状态:请在 KernelSU 管理器的「超级用户」里把 AdbHide 加入白名单"
                    + (error == null ? "" : "(" + error + ")"));
        } else {
            hint.setText("");
        }
        return sb.toString();
    }

    private Map<String, String> readSnapshot() {
        Map<String, String> out = new HashMap<>();
        try {
            File f = new File(Environment.getExternalStorageDirectory(),
                    "Android/data/com.blazkness.adbhide/files/status.txt");
            if (!f.exists()) {
                return null;
            }
            BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f)));
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) {
                    out.put(line.substring(0, eq), line.substring(eq + 1).trim());
                }
            }
            r.close();
        } catch (Throwable t) {
            return null;
        }
        return out.isEmpty() ? null : out;
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
