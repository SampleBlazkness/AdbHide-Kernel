package com.blazkness.adbhide;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.blazkness.adbng.R;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.ArrayList;

/**
 * The chrome that must never scroll: tab bar, repo link, update row. Both screens
 * build it through this one method at the same offsets, so switching tabs moves
 * nothing on screen.
 */
public final class Head {

    private static final String REPO = "https://github.com/SampleBlazkness/AdbHide-Kernel";
    private static final String API =
            "https://api.github.com/repos/SampleBlazkness/AdbHide-Kernel/releases/latest";

    /** Shared state, so a check started on one screen is already shown on the other. */
    private static final ArrayList<TextView> LABELS = new ArrayList<>();
    private static String updateText = "";
    private static String updateDetail = "";
    private static boolean checking;

    private Head() {
    }

    public static View bar(final Activity host, boolean onHide) {
        LinearLayout box = new LinearLayout(host);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackgroundColor(Ui.BG);
        box.setPadding(Ui.dp(host, 16), Ui.dp(host, 22), Ui.dp(host, 16), Ui.dp(host, 10));

        box.addView(Tabs.bar(host, onHide));

        TextView repo = Ui.sub(host, "github.com/SampleBlazkness/AdbHide-Kernel");
        repo.setIncludeFontPadding(false);
        repo.setTextColor(Ui.ACCENT);
        repo.setPadding(0, Ui.dp(host, 6), 0, 0);
        repo.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                try {
                    v.getContext().startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(REPO)));
                } catch (Throwable ignored) {
                }
            }
        });
        box.addView(repo);

        LinearLayout row = new LinearLayout(host);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, Ui.dp(host, 8), 0, 0);

        TextView label = Ui.text(host, updateText, 11, Ui.MUTED);
        label.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (updateDetail == null || updateDetail.isEmpty()) {
                    return;
                }
                new AlertDialog.Builder(host)
                        .setTitle(host.getString(R.string.update_detail_title))
                        .setMessage(updateDetail)
                        .setPositiveButton(host.getString(R.string.ok), null)
                        .show();
            }
        });
        row.addView(label, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        LABELS.add(label);

        TextView check = Ui.text(host, host.getString(R.string.check_update), 13, Ui.ACCENT);
        check.setPadding(Ui.dp(host, 10), Ui.dp(host, 6), Ui.dp(host, 2), Ui.dp(host, 6));
        check.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                check(host);
            }
        });
        row.addView(check);
        box.addView(row);

        if (updateText.isEmpty() && !checking) {
            check(host);
        }
        return box;
    }

    private static void publish(String text) {
        updateText = text;
        // 后台页面的 View 没有 windowToken，但那不代表它死了：照写就行
        for (int i = 0; i < LABELS.size(); i++) {
            TextView t = LABELS.get(i);
            if (t != null) {
                t.setText(text);
            }
        }
        while (LABELS.size() > 4) {
            LABELS.remove(0);
        }
    }

    static void check(final Activity host) {
        if (checking) {
            return;
        }
        checking = true;
        publish(host.getString(R.string.update_checking));
        new Thread(new Runnable() {
            @Override
            public void run() {
                String out;
                String detail = "";
                try {
                    String appVer = host.getPackageManager()
                            .getPackageInfo(host.getPackageName(), 0).versionName;
                    String kVer = moduleVersion("/data/adb/modules/adbhide_kmod/module.prop");
                    String nVer = moduleVersion("/data/adb/modules/adbhide_native/module.prop");
                    JSONObject o = new JSONObject(httpGet(API));
                    String tag = o.optString("tag_name", "").replace("v", "");
                    String kLatest = "";
                    String nLatest = "";
                    JSONArray assets = o.optJSONArray("assets");
                    if (assets != null) {
                        for (int i = 0; i < assets.length(); i++) {
                            String n = assets.getJSONObject(i).optString("name", "");
                            if (n.contains("kmod-v")) {
                                kLatest = between(n, "kmod-v", ".zip");
                            }
                            if (n.contains("native-v")) {
                                nLatest = between(n, "native-v", ".zip");
                            }
                        }
                    }
                    StringBuilder sb = new StringBuilder();
                    int hits = 0;
                    if (isNewer(tag, appVer)) {
                        hits++;
                        sb.append(host.getString(R.string.mod_app)).append(" ")
                                .append(appVer).append(" → ").append(tag).append("\n");
                    }
                    if (!kVer.isEmpty() && isNewer(kLatest, kVer)) {
                        hits++;
                        sb.append(host.getString(R.string.mod_kernel)).append(" ")
                                .append(kVer).append(" → ").append(kLatest).append("\n");
                    }
                    if (!nVer.isEmpty() && isNewer(nLatest, nVer)) {
                        hits++;
                        sb.append("Zygisk ").append(nVer).append(" → ").append(nLatest).append("\n");
                    }
                    detail = sb.toString().trim();
                    out = hits == 0 ? host.getString(R.string.update_latest)
                            : host.getString(R.string.update_available, hits);
                } catch (Throwable t) {
                    out = host.getString(R.string.update_failed);
                }
                final String text = out;
                final String det = detail;
                host.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        checking = false;
                        updateDetail = det;
                        publish(text);
                    }
                });
            }
        }).start();
    }

    private static String moduleVersion(String path) {
        Process p = null;
        try {
            p = Runtime.getRuntime().exec(new String[]{"su", "-c", "sed -n 's/^version=//p' " + path});
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line = r.readLine();
            r.close();
            p.waitFor();
            return line == null ? "" : line.trim().replace("v", "");
        } catch (Throwable t) {
            return "";
        } finally {
            if (p != null) {
                p.destroy();
            }
        }
    }

    private static String httpGet(String url) throws Exception {
        java.net.HttpURLConnection c =
                (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
        c.setConnectTimeout(8000);
        c.setReadTimeout(8000);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        StringBuilder sb = new StringBuilder();
        BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream()));
        String line;
        while ((line = r.readLine()) != null) {
            sb.append(line);
        }
        r.close();
        c.disconnect();
        return sb.toString();
    }

    private static String between(String s, String a, String b) {
        int i = s.indexOf(a);
        if (i < 0) {
            return "";
        }
        i += a.length();
        int j = s.indexOf(b, i);
        return j < 0 ? "" : s.substring(i, j);
    }

    private static boolean isNewer(String latest, String current) {
        if (latest == null || latest.isEmpty() || current == null || current.isEmpty()) {
            return false;
        }
        String[] a = latest.split("[.]");
        String[] b = current.split("[.]");
        for (int i = 0; i < Math.max(a.length, b.length); i++) {
            int x = i < a.length ? num(a[i]) : 0;
            int y = i < b.length ? num(b[i]) : 0;
            if (x != y) {
                return x > y;
            }
        }
        return false;
    }

    private static int num(String s) {
        try {
            return Integer.parseInt(s.trim());
        } catch (Throwable t) {
            return 0;
        }
    }
}