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

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.HashMap;
import java.util.Map;

/**
 * Status panel. Every value comes from the device itself: the kernel module's
 * /proc node, the KernelSU module directory, the iptables chain and the uid
 * list. Reading them needs root, so the app has to be allowed in the KernelSU
 * manager - KernelSU denies su for apps that are not on its list, there is no
 * prompt to accept.
 */
public class MainActivity extends Activity {

    private static final String SCRIPT =
            "p=/proc/adbhide_kmod\n" +
            "echo KMOD=$([ -r $p ] && echo 1 || echo 0)\n" +
            "if [ -r $p ]; then\n" +
            "  echo KVER=$(head -n1 $p | cut -d' ' -f2)\n" +
            "  echo KMODE=$(sed -n 's/^mode //p' $p)\n" +
            "  echo KPROBES=$(sed -n 's/^probes //p' $p)\n" +
            "  echo KCOUNTERS=$(sed -n 's/^counters //p' $p)\n" +
            "  echo KTARGETS=$(sed -n 's/^targets [0-9]*://p' $p)\n" +
            "  echo KREDIR=$(sed -n 's/^redirect //p' $p)\n" +
            "  echo KNETSEQ=$(sed -n 's/^netseq //p' $p)\n" +
            "fi\n" +
            "d=/data/adb/modules/adbhide_kmod\n" +
            "echo MODULE=$([ -d $d ] && echo 1 || echo 0)\n" +
            "echo MODDISABLED=$([ -e $d/disable ] && echo 1 || echo 0)\n" +
            "echo NATIVE=$([ -f /data/adb/modules/adbhide_native/zygisk/arm64-v8a.so ] && echo 1 || echo 0)\n" +
            "echo UIDS=$(tr '\\n' ' ' < /data/adb/adbhide/uids.txt 2>/dev/null)\n" +
            "echo PORTS=$(iptables -S ADBHIDE_PORTS 2>/dev/null | grep -c uid-owner)\n" +
            "echo MDNS=$(iptables -S ADBHIDE_MDNS_G 2>/dev/null | grep -c string)\\n";

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
        subtitle.setText("内核 + Native + LSPosed · 默认启用");
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
                    v.put(line.substring(0, eq), line.substring(eq + 1));
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

        boolean haveRoot = "1".equals(v.get("KMOD")) || "1".equals(v.get("MODULE"));

        StringBuilder sb = new StringBuilder();
        sb.append("内核模块 (kmod)\n");
        sb.append("  ").append(line(haveRoot, "1".equals(v.get("KMOD")),
                "已加载 " + nz(v.get("KVER")), "未加载")).append('\n');
        if (v.containsKey("KPROBES")) {
            sb.append("  探针 ").append(nz(v.get("KPROBES"))).append('\n');
        }
        if (v.containsKey("KREDIR")) {
            sb.append("  属性层 ").append(nz(v.get("KREDIR"))).append('\n');
        }
        if (v.containsKey("KNETSEQ")) {
            sb.append("  网络层 ").append(nz(v.get("KNETSEQ"))).append('\n');
        }
        if (v.containsKey("KCOUNTERS")) {
            sb.append("  计数 ").append(nz(v.get("KCOUNTERS"))).append('\n');
        }

        sb.append('\n');
        sb.append("Native 层 (Zygisk)\n");
        sb.append("  ").append(line(haveRoot, "1".equals(v.get("NATIVE")),
                "已安装 · 目标进程属性映射已私有化", "未安装")).append('\n');

        sb.append('\n');
        sb.append("网络层规则\n");
        sb.append("  ").append(haveRoot
                ? "adbd 端口 REJECT " + nz(v.get("PORTS")) + " 条 · mDNS 丢弃 " + nz(v.get("MDNS")) + " 条"
                : "无法读取(需要 root)").append('\n');

        sb.append('\n');
        sb.append("目标 uid\n");
        String uids = nz(v.get("UIDS"));
        sb.append("  ").append(uids.isEmpty() ? "(空)" : uids.trim()).append('\n');
        String kt = nz(v.get("KTARGETS"));
        if (!kt.isEmpty()) {
            sb.append("  内核已接收:").append(kt.trim()).append('\n');
        }

        sb.append('\n');
        sb.append("LSPosed 层\n");
        sb.append("  ").append(MainHook.appHooked
                ? "✅ 本进程已注入(SystemProperties 生效)"
                : "— 本进程未注入(如需按应用伪造属性,把本应用加入作用域)").append('\n');
        sb.append("  系统框架:作用域勾选「系统框架」后,\n");
        sb.append("  所有应用读 adb_enabled / development_settings_enabled\n");
        sb.append("  / adb_wifi_enabled 都会得到 0\n");

        if (!haveRoot) {
            sb.append('\n');
            hint.setText("读不到模块状态:请在 KernelSU 管理器的「超级用户」里把 AdbHide 加入白名单"
                    + (error == null ? "" : "(" + error + ")"));
        } else {
            hint.setText("");
        }
        return sb.toString();
    }

    private static String line(boolean known, boolean ok, String yes, String no) {
        if (!known) {
            return "— " + no;
        }
        return (ok ? "✅ " : "❌ ") + (ok ? yes : no);
    }

    private static String nz(String s) {
        return s == null ? "" : s;
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                getResources().getDisplayMetrics());
    }
}
