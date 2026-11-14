package com.blazkness.adbhide;

import android.app.Activity;
import android.app.Dialog;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.drawable.Drawable;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.CheckBox;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Full screen picker: icon + label + package, checkbox per app. */
final class AppPicker {

    private AppPicker() {
    }

    static final class Entry {
        String pkg;
        String label;
        Drawable icon;
        boolean system;
        boolean checked;
    }

    static void show(final Activity activity, final Runnable onDone) {
        final List<Entry> all = new ArrayList<Entry>();
        final List<Entry> entries = new ArrayList<Entry>();
        for (Entry e : all) {
            if (!e.system) {
                entries.add(e);
            }
        }
        final Dialog dialog = new Dialog(activity);
        dialog.setTitle(activity.getString(R.string.picker_title));

        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);

        final CheckBox showSystem = new CheckBox(activity);
        showSystem.setText(activity.getString(R.string.picker_show_system));
        showSystem.setChecked(false);
        showSystem.setPadding(Ui.dp(activity, 16), Ui.dp(activity, 10),
                Ui.dp(activity, 16), Ui.dp(activity, 4));
        root.addView(showSystem);

        final ListView list = new ListView(activity);
        list.setAdapter(new BaseAdapter() {
            @Override
            public int getCount() {
                return entries.size();
            }

            @Override
            public Object getItem(int position) {
                return entries.get(position);
            }

            @Override
            public long getItemId(int position) {
                return position;
            }

            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                Entry e = entries.get(position);
                LinearLayout row = new LinearLayout(activity);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(Ui.dp(activity, 16), Ui.dp(activity, 8),
                        Ui.dp(activity, 16), Ui.dp(activity, 8));

                ImageView icon = new ImageView(activity);
                icon.setImageDrawable(e.icon);
                LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(
                        Ui.dp(activity, 40), Ui.dp(activity, 40));
                ip.rightMargin = Ui.dp(activity, 12);
                row.addView(icon, ip);

                LinearLayout texts = new LinearLayout(activity);
                texts.setOrientation(LinearLayout.VERTICAL);
                TextView label = Ui.text(activity, e.label, 15, Ui.INK);
                TextView pkg = Ui.text(activity, e.pkg, 11, Ui.MUTED);
                texts.addView(label);
                texts.addView(pkg);
                row.addView(texts, new LinearLayout.LayoutParams(0,
                        LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

                CheckBox cb = new CheckBox(activity);
                cb.setChecked(e.checked);
                cb.setClickable(false);
                cb.setFocusable(false);
                row.addView(cb);

                final int pos = position;
                row.setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        entries.get(pos).checked = !entries.get(pos).checked;
                        notifyDataSetChanged();
                    }
                });
                return row;
            }
        });
        root.addView(list, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        new Thread(new Runnable() {
            @Override
            public void run() {
                final List<Entry> loaded = load(activity);
                activity.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        all.addAll(loaded);
                        entries.clear();
                        for (Entry e : all) {
                            if (showSystem.isChecked() || !e.system) {
                                entries.add(e);
                            }
                        }
                        ((BaseAdapter) list.getAdapter()).notifyDataSetChanged();
                    }
                });
            }
        }).start();

        showSystem.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                entries.clear();
                for (Entry e : all) {
                    if (showSystem.isChecked() || !e.system) {
                        entries.add(e);
                    }
                }
                list.setAdapter(list.getAdapter());
            }
        });

        LinearLayout actions = new LinearLayout(activity);
        actions.setPadding(Ui.dp(activity, 16), Ui.dp(activity, 10),
                Ui.dp(activity, 16), Ui.dp(activity, 14));

        TextView apply = Ui.action(activity, activity.getString(R.string.save_sync), true);
        apply.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Set<String> selected = new HashSet<String>();
                for (Entry e : all) {
                    if (e.checked) {
                        selected.add(e.pkg);
                    }
                }
                save(activity, selected);
                dialog.dismiss();
                if (onDone != null) {
                    onDone.run();
                }
            }
        });
        actions.addView(apply, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView cancel = Ui.action(activity, activity.getString(R.string.cancel), false);
        cancel.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                dialog.dismiss();
            }
        });
        LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        cp.leftMargin = Ui.dp(activity, 10);
        actions.addView(cancel, cp);

        root.addView(actions);
        dialog.setContentView(root);
        dialog.show();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(Color.WHITE));
            dialog.getWindow().setLayout(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.MATCH_PARENT);
        }
    }

    private static List<Entry> load(Activity activity) {
        PackageManager pm = activity.getPackageManager();
        Set<String> selected = readList();
        List<Entry> out = new ArrayList<Entry>();
        List<ApplicationInfo> apps = pm.getInstalledApplications(0);
        String self = activity.getPackageName();
        for (ApplicationInfo ai : apps) {
            if (self.equals(ai.packageName)) {
                continue;
            }
            Entry e = new Entry();
            e.pkg = ai.packageName;
            e.label = String.valueOf(pm.getApplicationLabel(ai));
            e.system = (ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
            try {
                e.icon = pm.getApplicationIcon(ai);
            } catch (Throwable ignored) {
            }
            e.checked = selected.contains(e.pkg);
            out.add(e);
        }
        Collections.sort(out, new Comparator<Entry>() {
            @Override
            public int compare(Entry a, Entry b) {
                if (a.checked != b.checked) {
                    return a.checked ? -1 : 1;
                }
                return a.label.compareToIgnoreCase(b.label);
            }
        });
        return out;
    }

    private static Set<String> readList() {
        Set<String> out = new HashSet<String>();
        Process p = null;
        try {
            p = Runtime.getRuntime().exec(new String[]{"su", "-c",
                    "cat /data/adb/adbhide/blacklist.conf"});
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (!line.isEmpty() && !line.startsWith("#")) {
                    out.add(line);
                }
            }
            r.close();
            p.waitFor();
        } catch (Throwable ignored) {
        } finally {
            if (p != null) {
                p.destroy();
            }
        }
        return out;
    }

    private static void save(Activity activity, Set<String> selected) {
        StringBuilder body = new StringBuilder("mkdir -p /data/adb/adbhide\n");
        body.append("f=/data/adb/adbhide/blacklist.conf\n");
        body.append(": > $f\n");
        String selfPkg = activity.getPackageName();
        for (String pkg : selected) {
            if (selfPkg.equals(pkg)) {
                continue;
            }
            body.append("echo '").append(pkg).append("' >> $f\n");
        }
        body.append("chmod 600 $f\n");
        body.append("sh /data/adb/modules/adbhide_kmod/sync.sh\n");
        try {
            Runtime.getRuntime().exec(new String[]{"su", "-c", body.toString()}).waitFor();
        } catch (Throwable ignored) {
        }
    }
}
