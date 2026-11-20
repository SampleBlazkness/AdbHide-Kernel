package com.blazkness.adbng;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Everything we know about adbng, read through adbngctl. */
final class Ctl {

    static final class Pending {
        String fp = "";
        String host = "";
        String since = "";
    }

    static final class Row {
        long ts;
        String stamp = "";
        String fp = "";
        String verdict = "";
        String host = "";
    }

    /** status prints key=value, so keep the order and let the UI pick. */
    static Map<String, String> status() {
        Map<String, String> m = new LinkedHashMap<>();
        Sh.R r = Sh.ctl("status");
        for (String line : r.out.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) {
                continue;
            }
            m.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
        }
        if (m.isEmpty()) {
            m.put("state", "unknown");
            m.put("error", r.text());
        }
        return m;
    }

    /** null when nobody is waiting for an answer. */
    static Pending pending() {
        Sh.R r = Sh.ctl("pending");
        Pending p = null;
        for (String line : r.out.split("\n")) {
            String s = line.trim();
            if (s.startsWith("--- ")) {
                p = new Pending();
                String f = s.substring(4);
                if (f.endsWith(".req")) {
                    f = f.substring(0, f.length() - 4);
                }
                p.fp = f;
            } else if (p != null && s.startsWith("host=")) {
                p.host = s.substring(5);
            } else if (p != null && s.startsWith("since=")) {
                p.since = s.substring(6);
            }
        }
        return p;
    }

    static List<Row> devices() {
        List<Row> list = new ArrayList<>();
        Sh.R r = Sh.run("cat " + Sh.DIR + "/devices.tsv 2>/dev/null");
        SimpleDateFormat fmt = new SimpleDateFormat("MM-dd HH:mm", Locale.US);
        for (String line : r.out.split("\n")) {
            String[] f = line.split("\t");
            if (f.length < 3) {
                continue;
            }
            Row row = new Row();
            try {
                row.ts = Long.parseLong(f[0].trim()) * 1000L;
            } catch (NumberFormatException ignored) {
                row.ts = 0;
            }
            row.fp = f[1].trim();
            row.verdict = f[2].trim();
            row.host = f.length > 3 ? f[3].trim() : "";
            row.stamp = fmt.format(new Date(row.ts == 0 ? System.currentTimeMillis() : row.ts));
            list.add(row);
        }
        // newest first, the UI only shows a handful
        for (int i = 0, j = list.size() - 1; i < j; i++, j--) {
            Row t = list.get(i);
            list.set(i, list.get(j));
            list.set(j, t);
        }
        return list;
    }

    static String logTail(int lines) {
        Sh.R r = Sh.run("tail -n " + lines + " " + Sh.DIR + "/adbng.log 2>/dev/null");
        return r.text();
    }
}