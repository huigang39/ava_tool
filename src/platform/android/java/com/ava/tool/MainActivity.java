package com.ava.tool;

import android.app.*;
import android.content.*;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.hardware.usb.*;
import android.net.Uri;
import android.os.*;
import android.text.*;
import android.view.*;
import android.view.inputmethod.InputMethodManager;
import android.widget.*;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BooleanSupplier;

/** Touch UI over the shared ELF/DWARF core and the direct USB SWD driver. */
public final class MainActivity extends Activity {
    private int BG, CARD, TEXT, MUTED, ACCENT, CONTAINER, OUTLINE, ON_ACCENT, ERROR;
    private int appearanceMode = AppearanceSchedule.SYSTEM, lightStart = 7 * 60, darkStart = 19 * 60;
    private boolean darkAppearance;
    private Context paletteContext;
    private LinearLayout rootShell, scheduleOptions;
    private Button appearanceButton, lightTimeButton, darkTimeButton;
    private final WeakHashMap<TextView, Integer> textColors = new WeakHashMap<>();
    private final WeakHashMap<View, Integer> flatSurfaces = new WeakHashMap<>(), roundedSurfaces = new WeakHashMap<>();
    private final WeakHashMap<Button, ActionStyle> buttonStyles = new WeakHashMap<>();
    private final WeakHashMap<EditText, Boolean> inputViews = new WeakHashMap<>();
    private final WeakHashMap<ImageButton, Boolean> moreButtons = new WeakHashMap<>();
    private final Handler appearanceHandler = new Handler(Looper.getMainLooper());
    private final Runnable appearanceTick = new Runnable() {
        @Override public void run() {
            if (!foreground || destroyed) return;
            applyAppearance(false);
            appearanceHandler.postDelayed(this, 60000 - Math.floorMod(System.currentTimeMillis(), 60000));
        }
    };
    private final ScrollView[] pages = new ScrollView[4];
    private final LinearLayout[] pageRoots = new LinearLayout[4];
    private final Button[] navigation = new Button[4];
    private int activePage;
    private TextView summary, pageTitle, feedback, scopeSummary;
    private ProgressBar progress;
    private String operation = "";
    private boolean permissionPending;
    private final LinearLayout[] settingsPanels = new LinearLayout[3];
    private final Button[] settingsTabs = new Button[3];
    private int activeSettings;
    private LinearLayout samplingBar;
    private Button samplingButton, channelButton;
    private TextView samplingLabel;
    private final TextView[] scopeValues = new TextView[3];
    private long lastScopeUpdate;
    private final ArrayList<ActionControl> actionControls = new ArrayList<>();
    private enum ActionStyle { PRIMARY, TONAL, TEXT, DANGER, STOP }
    private static final class ActionControl {
        final Button view; final BooleanSupplier available;
        ActionControl(Button view, BooleanSupplier available) { this.view = view; this.available = available; }
    }
    private android.window.OnBackInvokedCallback backCallback;
    private static final String USB_PERMISSION = "com.ava.tool.USB_PERMISSION";
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private final ExecutorService diagnosticWorker = Executors.newSingleThreadExecutor();
    private DeviceDiagnostics diagnostics;
    private final ArrayList<Watch> watches = new ArrayList<>();
    private final ArrayDeque<String> csvRows = new ArrayDeque<>();
    private UsbManager usbManager;
    private Jlink link; // worker thread only
    private volatile boolean destroyed, sampling, foreground;
    private boolean busy;
    private volatile boolean remoteSelected, remoteSampling;
    private TextView peerStatus;
    private Button shareButton;
    private String peerSnapshot = "";
    private final Handler peerHandler = new Handler(Looper.getMainLooper());
    private final Runnable peerTick = new Runnable() {
        @Override public void run() {
            if (!foreground || destroyed) return;
            String sharedStatus = PeerService.status + (PeerService.running ? " · " + NativePeer.viewers() + " 个连接" : "");
            String snapshot = sharedStatus + ":" + PeerService.ready() + ":" + remoteSelected + ":" + NativePeer.connected() + ":" + NativePeer.ownsControl();
            if (!snapshot.equals(peerSnapshot)) {
                peerSnapshot = snapshot;
                if (peerStatus != null) peerStatus.setText(sharedStatus);
                if (shareButton != null) shareButton.setText(PeerService.running ? "停止共享本机" : "共享本机 J-Link");
                refreshControls();
            }
            peerHandler.postDelayed(this, 1000);
        }
    };
    private volatile int generation;
    private ScheduledFuture<?> poller;
    private boolean hssSampling;
    private HssCapture hssCapture;
    private Watch hssWatch;
    private final java.util.concurrent.atomic.AtomicBoolean hssUiPending = new java.util.concurrent.atomic.AtomicBoolean();
    private TextView status, logs, countLabel;
    private LinearLayout channels;
    private EditText speedInput, apInput, periodInput, hssHzInput;
    private Plot plot;
    private JSONArray symbols = new JSONArray();
    private long started;
    private String pendingCsv;
    private UsbDevice activeDevice;
    private final StringBuilder logText = new StringBuilder();
    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if (Intent.ACTION_CONFIGURATION_CHANGED.equals(intent.getAction()) || Intent.ACTION_TIME_CHANGED.equals(intent.getAction()) || Intent.ACTION_TIMEZONE_CHANGED.equals(intent.getAction())) {
                if (foreground) applyAppearance(true);
                return;
            }
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (USB_PERMISSION.equals(intent.getAction())) {
                permissionPending = false; refreshControls();
                if (device != null && intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) connect(device);
                else message("USB 授权被拒绝，请重新连接并允许访问");
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction()) && device != null && device.equals(activeDevice)) {
                stopSampling(); activeDevice = null; message("J-Link 已拔出");
                worker.execute(() -> { closeLink(); });
            }
        }
    };
    static final class Sample {
        final double t, value;
        Sample(double t, double value) { this.t = t; this.value = value; }
    }
    static final class Watch {
        String name, type;
        long address;
        TextView valueView;
        LinearLayout cardView;
        final ArrayDeque<Sample> history = new ArrayDeque<>();
        Watch(String name, long address, String type) { this.name = name; this.address = address; this.type = type; }
    }
    interface Work { void run() throws Exception; }
    private int dp(float n) { return (int) (n * getResources().getDisplayMetrics().density + .5f); }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(appearanceContext()); view.setText(value); view.setTextSize(size); tintText(view, colorResource(color)); return view;
    }
    private int colorResource(int color) {
        if (color == TEXT) return R.color.text_primary;
        if (color == ACCENT) return R.color.accent;
        if (color == ERROR) return R.color.error;
        if (color == ON_ACCENT) return R.color.on_accent;
        return R.color.text_secondary;
    }
    private Context appearanceContext() {
        if (paletteContext != null) return paletteContext;
        Configuration config = new Configuration();
        config.uiMode = (getResources().getConfiguration().uiMode & ~Configuration.UI_MODE_NIGHT_MASK) |
            (darkAppearance ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO);
        // Keep the Activity as the base so dialogs inherit its window token.
        // A bare createConfigurationContext loses that token and crashes on show().
        ContextThemeWrapper themed = new ContextThemeWrapper(this, darkAppearance ? R.style.AppDarkTheme : R.style.AppLightTheme);
        themed.applyOverrideConfiguration(config);
        paletteContext = themed;
        return paletteContext;
    }
    private int appearanceColor(int resource) { return appearanceContext().getColor(resource); }
    private void tintText(TextView view, int resource) { textColors.put(view, resource); view.setTextColor(appearanceColor(resource)); }
    private void surface(View view, int resource, boolean rounded) {
        (rounded ? roundedSurfaces : flatSurfaces).put(view, resource);
        if (rounded) view.setBackground(background(appearanceColor(resource))); else view.setBackgroundColor(appearanceColor(resource));
    }
    private void loadPalette() {
        Context colors = appearanceContext();
        BG = colors.getColor(R.color.surface); CARD = colors.getColor(R.color.surface_card); TEXT = colors.getColor(R.color.text_primary);
        MUTED = colors.getColor(R.color.text_secondary); ACCENT = colors.getColor(R.color.accent);
        CONTAINER = colors.getColor(R.color.accent_container); OUTLINE = colors.getColor(R.color.outline);
        ON_ACCENT = colors.getColor(R.color.on_accent); ERROR = colors.getColor(R.color.error);
    }
    private boolean requestedDark() {
        Calendar clock = Calendar.getInstance();
        boolean system = (getApplicationContext().getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        return AppearanceSchedule.isDark(appearanceMode, clock.get(Calendar.HOUR_OF_DAY) * 60 + clock.get(Calendar.MINUTE), lightStart, darkStart, system);
    }
    private boolean inMainWindow(View view) { return rootShell != null && view.getRootView() == rootShell.getRootView(); }
    private void updateSystemBars() {
        getWindow().setStatusBarColor(BG); getWindow().setNavigationBarColor(BG);
        View decor = getWindow().getDecorView();
        int lightFlags = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        // Clear the theme's legacy light flags as well: on Android 11 they can
        // override InsetsController during a live light-to-dark switch.
        decor.setSystemUiVisibility(darkAppearance ? decor.getSystemUiVisibility() & ~lightFlags : decor.getSystemUiVisibility() | lightFlags);
        if (Build.VERSION.SDK_INT >= 30) {
            // Android 11's Window#getInsetsController dereferences an uncreated
            // decor view when called before setContentView during cold start.
            WindowInsetsController controller = decor.getWindowInsetsController();
            int mask = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
            if (controller != null) controller.setSystemBarsAppearance(darkAppearance ? 0 : mask, mask);
        }
    }
    private void applyAppearance(boolean force) {
        boolean dark = requestedDark();
        if (!force && dark == darkAppearance) return;
        darkAppearance = dark; paletteContext = null; loadPalette();
        setTheme(dark ? R.style.AppDarkTheme : R.style.AppLightTheme);
        // Existing dialogs keep their original theme until dismissed. Repaint the
        // live workspace only; no Activity recreation, sampling stop or USB reset.
        for (Map.Entry<TextView, Integer> item : textColors.entrySet())
            if (inMainWindow(item.getKey())) item.getKey().setTextColor(appearanceColor(item.getValue()));
        for (Map.Entry<View, Integer> item : flatSurfaces.entrySet())
            if (inMainWindow(item.getKey())) item.getKey().setBackgroundColor(appearanceColor(item.getValue()));
        for (Map.Entry<View, Integer> item : roundedSurfaces.entrySet())
            if (inMainWindow(item.getKey())) item.getKey().setBackground(background(appearanceColor(item.getValue())));
        for (Map.Entry<Button, ActionStyle> item : buttonStyles.entrySet())
            if (inMainWindow(item.getKey())) styleButton(item.getKey(), item.getValue());
        for (EditText edit : inputViews.keySet()) if (inMainWindow(edit)) tintInput(edit);
        for (ImageButton more : moreButtons.keySet()) if (inMainWindow(more)) tintMoreButton(more);
        if (progress != null) progress.setIndeterminateTintList(ColorStateList.valueOf(ACCENT));
        if (rootShell != null) {
            paintNavigation(); paintSettings(); updateWatchSelection(); refreshControls(); plot.invalidate();
        }
        updateSystemBars(); updateAppearanceLabels();
    }
    private void saveAppearance() {
        getSharedPreferences("appearance", MODE_PRIVATE).edit().putInt("mode", appearanceMode).putInt("lightStart", lightStart).putInt("darkStart", darkStart).apply();
        applyAppearance(true);
    }
    private String clockLabel(int minute) { return String.format(Locale.ROOT, "%02d:%02d", minute / 60, minute % 60); }
    private void updateAppearanceLabels() {
        if (appearanceButton == null) return;
        appearanceButton.setText("外观：" + new String[]{"跟随系统", "按时间自动", "浅色", "深色"}[appearanceMode]);
        lightTimeButton.setText("浅色开始  " + clockLabel(lightStart)); darkTimeButton.setText("深色开始  " + clockLabel(darkStart));
        scheduleOptions.setVisibility(appearanceMode == AppearanceSchedule.SCHEDULE ? View.VISIBLE : View.GONE);
    }
    private void chooseAppearanceTime(boolean light) {
        int time = light ? lightStart : darkStart;
        new TimePickerDialog(appearanceContext(), (picker, hour, minute) -> {
            int next = hour * 60 + minute;
            if (next == (light ? darkStart : lightStart)) { Toast.makeText(this, "浅色和深色开始时间不能相同", Toast.LENGTH_LONG).show(); return; }
            if (light) lightStart = next; else darkStart = next; saveAppearance();
        }, time / 60, time % 60, true).show();
    }
    private void drawAppearanceSettings(LinearLayout parent) {
        LinearLayout card = section(parent, "外观");
        appearanceButton = button(card, "", ActionStyle.TONAL, () -> new AlertDialog.Builder(appearanceContext()).setTitle("选择外观")
            .setSingleChoiceItems(new String[]{"跟随系统", "按时间自动", "浅色", "深色"}, appearanceMode, (dialog, which) -> {
                appearanceMode = which; dialog.dismiss(); saveAppearance();
            }).setNegativeButton("取消", null).show());
        scheduleOptions = row(); card.addView(scheduleOptions);
        lightTimeButton = button(scheduleOptions, "", ActionStyle.TEXT, () -> chooseAppearanceTime(true));
        darkTimeButton = button(scheduleOptions, "", ActionStyle.TEXT, () -> chooseAppearanceTime(false));
        card.addView(text("切换外观不会中断采样。", 12, MUTED));
        updateAppearanceLabels();
    }
    private GradientDrawable background(int color) {
        GradientDrawable d = new GradientDrawable(); d.setColor(color); d.setCornerRadius(dp(20)); return d;
    }
    private LinearLayout column() { LinearLayout l = new LinearLayout(this); l.setOrientation(LinearLayout.VERTICAL); return l; }
    private LinearLayout row() {
        LinearLayout l = new LinearLayout(this);
        Configuration c = getResources().getConfiguration();
        l.setOrientation(c.screenWidthDp < 360 || c.fontScale > 1.3f ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        return l;
    }
    private LinearLayout page(FrameLayout host, int index) {
        LinearLayout frame = column(); pageRoots[index] = frame;
        host.addView(frame, new FrameLayout.LayoutParams(-1, -1));
        ScrollView scroll = new ScrollView(this); scroll.setFillViewport(true); pages[index] = scroll;
        LinearLayout content = column(); content.setPadding(dp(16), 0, dp(16), dp(20)); scroll.addView(content);
        frame.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1)); return content;
    }
    private void selectPage(int index) {
        View focused = getCurrentFocus();
        if (focused != null) {
            ((InputMethodManager)getSystemService(INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(focused.getWindowToken(), 0);
            focused.clearFocus();
        }
        activePage = Math.max(0, Math.min(index, pages.length - 1));
        if (pageTitle != null) pageTitle.setText(new String[]{"设备", "变量", "示波器", "设置"}[activePage]);
        paintNavigation();
        refreshSamplingBar();
        // Let the system handle leaving the start page. Other pages return home.
        if (Build.VERSION.SDK_INT >= 33) {
            if (backCallback != null) { getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback); backCallback = null; }
            if (activePage != 0) {
                backCallback = () -> selectPage(0);
                getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
            }
        }
    }
    private void paintNavigation() {
        for (int i = 0; i < pages.length; i++) {
            pageRoots[i].setVisibility(i == activePage ? View.VISIBLE : View.GONE);
            navigation[i].setSelected(i == activePage);
            navigation[i].setTextColor(i == activePage ? ACCENT : MUTED);
            navigation[i].setCompoundDrawableTintList(ColorStateList.valueOf(i == activePage ? ACCENT : MUTED));
            navigation[i].setBackground(new RippleDrawable(ColorStateList.valueOf(withAlpha(ACCENT, 32)),
                background(i == activePage ? CONTAINER : Color.TRANSPARENT), null));
        }
    }
    @Override public void onBackPressed() { if (activePage != 0) selectPage(0); else super.onBackPressed(); }
    @Override protected void onSaveInstanceState(Bundle state) {
        state.putInt("page", activePage); state.putInt("settings", activeSettings);
        for (int i = 0; i < pages.length; ++i) state.putInt("scroll" + i, pages[i].getScrollY());
        super.onSaveInstanceState(state);
    }
    private LinearLayout section(LinearLayout root, String title) {
        LinearLayout box = column(); box.setPadding(dp(14), dp(14), dp(14), dp(14)); surface(box, R.color.surface_card, true);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2); p.setMargins(0, dp(12), 0, 0); root.addView(box, p);
        TextView heading = text(title, 18, TEXT); heading.setTypeface(null, Typeface.BOLD); heading.setPadding(0, 0, 0, dp(8));
        if (Build.VERSION.SDK_INT >= 28) heading.setAccessibilityHeading(true); box.addView(heading);
        return box;
    }
    private int withAlpha(int color, int alpha) { return (color & 0x00ffffff) | (alpha << 24); }
    private void styleButton(Button b, ActionStyle style) {
        buttonStyles.put(b, style);
        int fill = style == ActionStyle.STOP ? appearanceColor(R.color.stop_container) : style == ActionStyle.PRIMARY ? ACCENT : style == ActionStyle.TONAL ? CONTAINER : Color.TRANSPARENT;
        int ink = style == ActionStyle.STOP ? appearanceColor(R.color.on_stop_container) : style == ActionStyle.PRIMARY ? ON_ACCENT : style == ActionStyle.DANGER ? ERROR : ACCENT;
        b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{withAlpha(TEXT, 96), ink}));
        GradientDrawable shape = background(fill); shape.setCornerRadius(dp(24));
        shape.setColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{withAlpha(TEXT, 12), fill}));
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(withAlpha(ink, 32)), shape, null));
        b.setStateListAnimator(null);
    }
    private Button button(LinearLayout parent, String name, Runnable click) {
        return button(parent, name, ActionStyle.TONAL, click);
    }
    private Button button(LinearLayout parent, String name, ActionStyle style, Runnable click) {
        Button b = new Button(appearanceContext()); b.setText(name); b.setTextSize(14); b.setAllCaps(false); b.setTextColor(ACCENT);
        b.setSingleLine(false); b.setEllipsize(null); b.setHorizontallyScrolling(false);
        b.setMinHeight(dp(48)); b.setMinimumHeight(dp(48)); b.setMinWidth(0); b.setMinimumWidth(0);
        b.setPadding(dp(12), dp(10), dp(12), dp(10));
        styleButton(b, style);
        b.setOnClickListener(v -> click.run());
        LinearLayout.LayoutParams params = parent.getOrientation() == LinearLayout.HORIZONTAL ? new LinearLayout.LayoutParams(0, -2, 1) : new LinearLayout.LayoutParams(-1, -2);
        params.setMargins(dp(3), dp(6), dp(3), dp(6)); parent.addView(b, params);
        return b;
    }
    private Button action(LinearLayout parent, String name, ActionStyle style, BooleanSupplier available, Runnable click) {
        Button b = button(parent, name, style, click); actionControls.add(new ActionControl(b, available)); return b;
    }
    private boolean connected() { return remoteSelected ? NativePeer.connected() : PeerService.ready() || activeDevice != null; }
    private boolean idle() { return !busy && !permissionPending; }
    private boolean canRead() { return idle() && connected() && !sampling && !watches.isEmpty(); }
    private void refreshControls() {
        if (summary == null) return;
        for (ActionControl control : actionControls) {
            boolean enabled = control.available.getAsBoolean(); control.view.setEnabled(enabled);
        }
        String label = permissionPending ? "等待 USB 授权" : busy ? operation + "…" : sampling ? (hssSampling ? "HSS 采集中" : "正在采样") : connected() ? "J-Link 已连接" : "J-Link 未连接";
        summary.setText(remoteSelected ? "远程设备 · " + (NativePeer.connected() ? (sampling ? "采样中" : "已连接") : "已断开") : label); tintText(summary, connected() || busy ? R.color.accent : R.color.text_secondary);
        if (progress != null) progress.setVisibility(busy || permissionPending ? View.VISIBLE : View.INVISIBLE);
        for (EditText edit : new EditText[]{speedInput, apInput, periodInput, hssHzInput})
            if (edit != null) edit.setEnabled(idle() && !sampling && ((edit != speedInput && edit != apInput) || !connected()));
        if (scopeSummary != null && plot != null) {
            Watch selected = plot.selected;
            scopeSummary.setText(selected == null ? "添加变量后即可查看曲线" :
                String.format(Locale.ROOT, "%s · 0x%08X", selected.type, selected.address));
            channelButton.setText(selected == null ? "选择变量  ▾" : selected.name + "  ▾");
            refreshScopeValues(true);
        }
        refreshSamplingBar();
    }
    private void refreshSamplingBar() {
        if (samplingBar == null) return;
        samplingBar.setVisibility(sampling || activePage == 1 || activePage == 2 ? View.VISIBLE : View.GONE);
        samplingButton.setText(sampling ? "停止采样" : "开始采样");
        styleButton(samplingButton, sampling ? ActionStyle.STOP : ActionStyle.PRIMARY);
        samplingButton.setEnabled(sampling || canRead());
        samplingLabel.setText(sampling ? (hssSampling ? "HSS 采集中" : "采样中") : !connected() ? "请先连接设备" : watches.isEmpty() ? "请先添加变量" : "准备就绪");
    }
    private void paintSettings() {
        for (int i = 0; i < settingsPanels.length; i++) if (settingsPanels[i] != null) {
            settingsPanels[i].setVisibility(i == activeSettings ? View.VISIBLE : View.GONE);
            settingsTabs[i].setSelected(i == activeSettings);
            styleButton(settingsTabs[i], i == activeSettings ? ActionStyle.TONAL : ActionStyle.TEXT);
        }
    }
    private void openSettings(int tab) {
        activeSettings = Math.max(0, Math.min(tab, settingsPanels.length - 1)); paintSettings(); selectPage(3);
    }
    private void chooseChannel() {
        if (watches.isEmpty()) { selectPage(1); return; }
        String[] names = new String[watches.size()]; int checked = -1;
        for (int i = 0; i < names.length; i++) { names[i] = watches.get(i).name; if (watches.get(i) == plot.selected) checked = i; }
        new AlertDialog.Builder(appearanceContext()).setTitle("显示通道").setSingleChoiceItems(names, checked, (dialog, index) -> {
            plot.selected = watches.get(index); updateWatchSelection(); plot.invalidate(); refreshControls(); dialog.dismiss();
        }).setNegativeButton("取消", null).show();
    }
    private void scopeMenu(View anchor) {
        PopupMenu menu = new PopupMenu(appearanceContext(), anchor);
        menu.getMenu().add(0, 1, 0, "读取一次").setEnabled(canRead());
        menu.getMenu().add(0, 2, 1, "采样设置");
        menu.getMenu().add(0, 3, 2, "导出普通采样 CSV").setEnabled(idle() && !sampling && !csvRows.isEmpty());
        menu.getMenu().add(0, 4, 3, "导出 HSS CSV").setEnabled(idle() && !sampling && hssCapture != null);
        menu.getMenu().add(0, 5, 4, "清空普通采样记录…").setEnabled(idle() && !sampling && !csvRows.isEmpty());
        menu.setOnMenuItemClickListener(item -> {
            switch (item.getItemId()) {
                case 1: readOnce(); break;
                case 2: openSettings(0); break;
                case 3: exportCsv(); break;
                case 4: exportHssCsv(); break;
                case 5: clearRecords(); break;
                default: return false;
            }
            return true;
        }); menu.show();
    }
    private void refreshScopeValues(boolean force) {
        if (plot == null || scopeValues[0] == null) return;
        long now = SystemClock.elapsedRealtime(); if (!force && now - lastScopeUpdate < 100) return;
        lastScopeUpdate = now;
        Watch selected = plot.selected;
        double min = Double.POSITIVE_INFINITY, max = Double.NEGATIVE_INFINITY;
        if (selected != null) for (Sample sample : selected.history) if (Double.isFinite(sample.value)) { min = Math.min(min, sample.value); max = Math.max(max, sample.value); }
        double current = selected == null || selected.history.isEmpty() ? Double.NaN : selected.history.getLast().value;
        double[] values = {current, min, max};
        for (int i = 0; i < values.length; i++) scopeValues[i].setText(Double.isFinite(values[i]) ? String.format(Locale.ROOT, "%.5g", values[i]) : "—");
    }
    private void clearRecords() {
        new AlertDialog.Builder(appearanceContext()).setTitle("清空采样记录？").setMessage("将清空普通采样记录和当前曲线。变量列表与 HSS 记录保留。")
            .setNegativeButton("取消", null).setPositiveButton("清空记录", (d, which) -> {
                stopSampling(); csvRows.clear(); for (Watch w : watches) w.history.clear(); started = SystemClock.elapsedRealtimeNanos();
                plot.invalidate(); countLabel.setText(watches.size() + " 个变量 · 记录已清空"); refreshControls();
            }).show();
    }
    private EditText input(LinearLayout parent, String label, String value, int inputType) {
        LinearLayout box = column();
        if (parent.getOrientation() == LinearLayout.HORIZONTAL) parent.addView(box, new LinearLayout.LayoutParams(0, -2, 1)); else parent.addView(box);
        box.addView(text(label, 12, MUTED));
        EditText edit = new EditText(appearanceContext()); edit.setSingleLine(true); edit.setTextColor(TEXT); edit.setTextSize(16); edit.setMinHeight(dp(48)); edit.setInputType(inputType); edit.setText(value); edit.setSelectAllOnFocus(true); edit.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE); box.addView(edit);
        inputViews.put(edit, true); tintInput(edit);
        edit.setId(View.generateViewId()); ((TextView) box.getChildAt(0)).setLabelFor(edit.getId());
        return edit;
    }
    private void tintInput(EditText edit) {
        edit.setTextColor(TEXT); edit.setHintTextColor(MUTED);
        edit.setBackgroundTintList(new ColorStateList(new int[][]{new int[]{android.R.attr.state_focused}, new int[]{}}, new int[]{ACCENT, OUTLINE}));
    }
    private void tintMoreButton(ImageButton more) {
        more.setImageTintList(ColorStateList.valueOf(MUTED));
        more.setBackground(new RippleDrawable(ColorStateList.valueOf(withAlpha(ACCENT, 32)), background(Color.TRANSPARENT), null));
    }
    @Override protected void attachBaseContext(Context base) {
        SharedPreferences appearance = base.getSharedPreferences("appearance", MODE_PRIVATE);
        appearanceMode = Math.max(AppearanceSchedule.SYSTEM, Math.min(AppearanceSchedule.DARK, appearance.getInt("mode", AppearanceSchedule.SYSTEM)));
        lightStart = appearance.getInt("lightStart", 7 * 60); darkStart = appearance.getInt("darkStart", 19 * 60);
        if (lightStart < 0 || lightStart >= 1440 || darkStart < 0 || darkStart >= 1440 || lightStart == darkStart) { lightStart = 7 * 60; darkStart = 19 * 60; }
        Calendar clock = Calendar.getInstance();
        Configuration system = base.getResources().getConfiguration();
        darkAppearance = AppearanceSchedule.isDark(appearanceMode, clock.get(Calendar.HOUR_OF_DAY) * 60 + clock.get(Calendar.MINUTE), lightStart, darkStart,
            (system.uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES);
        // Apply before Activity resources/theme are first resolved. Later switches
        // repaint the workspace and never recreate this hardware-owning Activity.
        Configuration override = new Configuration();
        override.uiMode = (system.uiMode & ~Configuration.UI_MODE_NIGHT_MASK) | (darkAppearance ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO);
        super.attachBaseContext(base.createConfigurationContext(override));
    }
    @Override public void onCreate(Bundle state) {
        setTheme(darkAppearance ? R.style.AppDarkTheme : R.style.AppLightTheme);
        super.onCreate(state);
        loadPalette();
        diagnostics = new DeviceDiagnostics(new File(getFilesDir(), "connection-journal.txt"));
        if (Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(false);
        updateSystemBars();
        usbManager = (UsbManager) getSystemService(USB_SERVICE);
        LinearLayout shell = column(); rootShell = shell; surface(shell, R.color.surface, false); setContentView(shell);
        shell.post(this::updateSystemBars);
        shell.setOnApplyWindowInsetsListener((v, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                Insets keyboard = insets.getInsets(WindowInsets.Type.ime());
                v.setPadding(bars.left, bars.top, bars.right, Math.max(bars.bottom, keyboard.bottom));
            } else v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        LinearLayout appBar = new LinearLayout(this); appBar.setGravity(Gravity.CENTER_VERTICAL); appBar.setPadding(dp(20), dp(8), dp(16), dp(4)); shell.addView(appBar);
        pageTitle = text("设备", 24, TEXT); pageTitle.setTypeface(null, Typeface.BOLD); appBar.addView(pageTitle, new LinearLayout.LayoutParams(0, -2, 1));
        TextView brand = text("AvA Tool", 13, MUTED); appBar.addView(brand);
        summary = text("未连接", 13, MUTED); summary.setPadding(dp(20), 0, dp(20), dp(8)); summary.setMaxLines(1); summary.setEllipsize(TextUtils.TruncateAt.END); shell.addView(summary);
        summary.setOnClickListener(v -> selectPage(0)); summary.setContentDescription("连接状态，点击打开设备页");
        summary.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); progress.setIndeterminate(true); shell.addView(progress, new LinearLayout.LayoutParams(-1, dp(2)));
        feedback = text("", 13, MUTED); feedback.setPadding(dp(20), dp(8), dp(20), dp(8)); feedback.setMaxLines(3); feedback.setEllipsize(TextUtils.TruncateAt.END);
        feedback.setVisibility(View.GONE); feedback.setOnClickListener(v -> openSettings(2)); feedback.setContentDescription("最近操作反馈，点击查看完整日志"); shell.addView(feedback);
        LinearLayout workspace = new LinearLayout(this); workspace.setOrientation(LinearLayout.HORIZONTAL); shell.addView(workspace, new LinearLayout.LayoutParams(-1, 0, 1));
        boolean rail = getResources().getConfiguration().screenWidthDp >= 600;
        LinearLayout nav = new LinearLayout(this); nav.setOrientation(rail ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        nav.setPadding(dp(8), dp(4), dp(8), dp(4)); surface(nav, R.color.surface_card, false);
        if (rail) {
            ScrollView railScroll = new ScrollView(this); railScroll.setFillViewport(true); surface(railScroll, R.color.surface_card, false);
            railScroll.addView(nav); workspace.addView(railScroll, new LinearLayout.LayoutParams(dp(104), -1));
        }
        FrameLayout host = new FrameLayout(this); workspace.addView(host, new LinearLayout.LayoutParams(0, -1, 1));
        LinearLayout devicePage = page(host, 0), variablePage = page(host, 1), scopePage = page(host, 2), settingsPage = page(host, 3);
        LinearLayout root=column(),remotePage=column();devicePage.addView(root);devicePage.addView(remotePage);remotePage.setVisibility(View.GONE);
        LinearLayout deviceTabs=new LinearLayout(this);deviceTabs.setPadding(dp(16),0,dp(16),0);pageRoots[0].addView(deviceTabs,0);
        Button localTab=button(deviceTabs,"本机设备",ActionStyle.TONAL,()->{}),remoteTab=button(deviceTabs,"远程设备",ActionStyle.TEXT,()->{});
        localTab.setOnClickListener(v->{root.setVisibility(View.VISIBLE);remotePage.setVisibility(View.GONE);styleButton(localTab,ActionStyle.TONAL);styleButton(remoteTab,ActionStyle.TEXT);pages[0].scrollTo(0,0);});
        remoteTab.setOnClickListener(v->{root.setVisibility(View.GONE);remotePage.setVisibility(View.VISIBLE);styleButton(localTab,ActionStyle.TEXT);styleButton(remoteTab,ActionStyle.TONAL);pages[0].scrollTo(0,0);});
        samplingBar = new LinearLayout(this); samplingBar.setGravity(Gravity.CENTER_VERTICAL); samplingBar.setPadding(dp(20), dp(4), dp(16), dp(4)); surface(samplingBar, R.color.surface_card, false);
        samplingLabel = text("请先连接设备", 13, MUTED); samplingBar.addView(samplingLabel, new LinearLayout.LayoutParams(0, -2, 1));
        samplingButton = button(samplingBar, "开始采样", ActionStyle.PRIMARY, () -> { if (sampling) stopSampling(); else startSampling(); });
        samplingButton.setLayoutParams(new LinearLayout.LayoutParams(dp(132), -2)); shell.addView(samplingBar);
        if (!rail) shell.addView(nav);
        String[] destinations = {"设备", "变量", "示波器", "设置"};
        int[] icons = {R.drawable.ic_connection, R.drawable.ic_variables, R.drawable.ic_scope, R.drawable.ic_settings};
        for (int i = 0; i < navigation.length; i++) {
            final int target = i; navigation[i] = button(nav, destinations[i], () -> selectPage(target));
            navigation[i].setMinHeight(dp(64)); navigation[i].setPadding(dp(4), dp(6), dp(4), dp(6)); navigation[i].setTextSize(12); navigation[i].setContentDescription(destinations[i] + "页面");
            navigation[i].setCompoundDrawablesWithIntrinsicBounds(0, icons[i], 0, 0); navigation[i].setCompoundDrawablePadding(dp(4));
        }
        selectPage(state == null ? 0 : state.getInt("page", 0));
        if (state != null) for (int i = 0; i < pages.length; ++i) {
            final ScrollView p = pages[i]; final int y = state.getInt("scroll" + i, 0); p.post(() -> p.scrollTo(0, y));
        }
        LinearLayout settingsNav = new LinearLayout(this); settingsNav.setPadding(dp(16), dp(4), dp(16), 0); pageRoots[3].addView(settingsNav, 0);
        String[] settingsNames = {"采样", "外观", "日志"};
        for (int i = 0; i < settingsPanels.length; i++) {
            final int target = i; settingsTabs[i] = button(settingsNav, settingsNames[i], () -> { activeSettings = target; paintSettings(); pages[3].scrollTo(0, 0); });
            settingsPanels[i] = column(); settingsPage.addView(settingsPanels[i]);
        }
        activeSettings = state == null ? 0 : Math.max(0, Math.min(2, state.getInt("settings", 0))); paintSettings();
        LinearLayout logPage = settingsPanels[2];
        LinearLayout connection = section(root, "J-Link");
        status = text("通过 OTG 接入探针，为目标板供电。", 14, MUTED); status.setPadding(0, 0, 0, dp(12)); status.setTextIsSelectable(true); connection.addView(status);
        LinearLayout settings = row(); connection.addView(settings);
        SharedPreferences prefs = getPreferences(MODE_PRIVATE);
        speedInput = input(settings, "SWD / kHz", prefs.getString("speed", "1000"), 2);
        apInput = input(settings, "AHB AP", prefs.getString("ap", "0"), 2);
        LinearLayout connectButtons = row(); connection.addView(connectButtons);
        action(connectButtons, "连接 J-Link", ActionStyle.PRIMARY, () -> idle() && !PeerService.running && !remoteSelected && !connected(), this::chooseDevice);
        action(connectButtons, "断开连接", ActionStyle.TEXT, () -> idle() && connected() && (remoteSelected || !PeerService.running), () -> job("断开", () -> { if (remoteSelected) NativePeer.close(); else closeLink(); ui(() -> message("已断开")); }));
        action(connection, "复位目标 MCU…", ActionStyle.DANGER, () -> idle() && connected() && !sampling && !remoteSelected, () -> new AlertDialog.Builder(appearanceContext()).setTitle("复位目标 MCU？").setMessage("将通过 AIRCR 请求系统复位。复位后需要重新连接。").setNegativeButton("取消", null).setPositiveButton("复位", (d, w) -> job("复位", () -> { resetSelected(); ui(() -> message("已请求 MCU 复位，请重新连接")); })).show());
        drawPeerControls(remotePage,root);
        LinearLayout steps = section(root, "下一步");
        button(steps, "添加变量  →", ActionStyle.TONAL, () -> selectPage(1));
        button(steps, "连接帮助", ActionStyle.TEXT, () -> new AlertDialog.Builder(appearanceContext()).setTitle("第一次使用")
            .setMessage("1. 使用 OTG 连接 J-Link，并单独给目标板供电。\n\n2. 连接成功后，在变量页导入 ELF / AXF 或手动添加地址。\n\n3. 点击底部“开始采样”，在示波器页切换通道。\n\n切到其他应用会停止采样并释放 USB。")
            .setPositiveButton("知道了", null).show());
        drawAppearanceSettings(settingsPanels[1]);
        LinearLayout variables = section(variablePage, "监控变量");
        LinearLayout importButtons = row(); variables.addView(importButtons);
        action(importButtons, "导入 ELF / AXF", ActionStyle.PRIMARY, () -> idle() && !sampling, () -> { stopSampling(); Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE); startActivityForResult(i, 10); });
        action(importButtons, "手动添加", ActionStyle.TONAL, () -> idle() && !sampling && watches.size() < 32, () -> addDialog("", "", "f32"));
        action(variables, "浏览已导入的符号", ActionStyle.TEXT, () -> idle() && !sampling && symbols.length() > 0, this::showSymbols);
        countLabel = text("0 个变量 · 未采样", 12, MUTED); variables.addView(countLabel);
        channels = column(); variables.addView(channels);
        LinearLayout acquisition = section(settingsPanels[0], "普通采样");
        periodInput = input(acquisition, "每轮采样后等待 / ms", prefs.getString("period", "100"), 2);
        acquisition.addView(text("范围 20–60000 ms。采样按实际读取时刻记录。", 12, MUTED));
        LinearLayout runButtons = row(); acquisition.addView(runButtons);
        button(runButtons, "前往示波器", ActionStyle.TONAL, () -> selectPage(2));
        action(runButtons, "读取一次", ActionStyle.TONAL, this::canRead, this::readOnce);
        LinearLayout hss = section(settingsPanels[0], "HSS · 实验功能");
        hss.addView(text("仅支持已验证的 Pro V4 / 2022-09-22 固件、H74x M7 AP0。当前曲线必须是单个 32 位 RAM 变量。停止 HSS 后需重新连接。", 13, MUTED));
        hssHzInput = input(hss, "目标频率 / Hz", prefs.getString("hssHz", "25000"), 2);
        action(hss, "开始 HSS 高速采样", ActionStyle.TONAL, () -> canRead() && !remoteSelected && !PeerService.running && plot != null && plot.selected != null && Values.size(plot.selected.type) == 4 && JlinkHss.supportedRam(plot.selected.address), this::startHssSampling);
        action(hss, "导出 HSS CSV", ActionStyle.TEXT, () -> idle() && !sampling && hssCapture != null, this::exportHssCsv);
        hss.addView(text("20 kHz 实测目标建议 SWD 设为 12000 kHz、HSS 设为 25000 Hz。曲线降采样显示；CSV 保留缓冲区内最近 20 万个原始点。", 12, MUTED));
        LinearLayout channelRow = new LinearLayout(this); channelRow.setGravity(Gravity.CENTER_VERTICAL); scopePage.addView(channelRow);
        channelButton = button(channelRow, "选择变量  ▾", ActionStyle.TONAL, this::chooseChannel);
        channelButton.setMaxLines(2); channelButton.setEllipsize(TextUtils.TruncateAt.MIDDLE); channelButton.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        ImageButton scopeMore = new ImageButton(appearanceContext()); scopeMore.setImageResource(R.drawable.ic_more); scopeMore.setContentDescription("示波器操作：采样设置、读取、导出、清空");
        moreButtons.put(scopeMore, true); tintMoreButton(scopeMore); channelRow.addView(scopeMore, new LinearLayout.LayoutParams(dp(48), dp(48))); scopeMore.setOnClickListener(this::scopeMenu);
        LinearLayout graph = column(); graph.setPadding(dp(12), dp(12), dp(12), dp(12)); surface(graph, R.color.surface_card, true); scopePage.addView(graph);
        scopeSummary = text("", 12, MUTED); graph.addView(scopeSummary);
        plot = new Plot(); graph.addView(plot, new LinearLayout.LayoutParams(-1, dp(Math.max(180, Math.min(480, getResources().getConfiguration().screenHeightDp - 400)))));
        LinearLayout metrics = row(); graph.addView(metrics);
        String[] metricNames = {"当前值", "最小值", "最大值"};
        for (int i = 0; i < metricNames.length; i++) {
            LinearLayout metric = column(); metric.setPadding(dp(4), dp(8), dp(4), dp(8));
            metrics.addView(metric, metrics.getOrientation() == LinearLayout.HORIZONTAL ? new LinearLayout.LayoutParams(0, -2, 1) : new LinearLayout.LayoutParams(-1, -2));
            metric.addView(text(metricNames[i], 12, MUTED)); scopeValues[i] = text("—", 17, TEXT); metric.addView(scopeValues[i]);
        }
        TextView statsNote = text("统计范围：当前曲线的显示点", 11, MUTED); graph.addView(statsNote);
        Button scopeSettings = button(scopePage, "采样设置", ActionStyle.TEXT, () -> openSettings(0));
        pages[2].addOnLayoutChangeListener((v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
            // Use the actual viewport after system bars/navigation, not an assumed phone height.
            // At large font sizes retain a usable plot and let the page scroll instead of clipping.
            if (bottom <= top || channelRow.getHeight() == 0) return;
            LinearLayout.LayoutParams settingsParams = (LinearLayout.LayoutParams) scopeSettings.getLayoutParams();
            int other = scopePage.getPaddingTop() + scopePage.getPaddingBottom() + channelRow.getHeight()
                + graph.getPaddingTop() + graph.getPaddingBottom() + scopeSummary.getHeight() + metrics.getHeight() + statsNote.getHeight()
                + scopeSettings.getHeight() + settingsParams.topMargin + settingsParams.bottomMargin;
            int height = Math.max(dp(180), Math.min(dp(480), bottom - top - other));
            if (plot.getLayoutParams().height != height) { plot.getLayoutParams().height = height; plot.requestLayout(); }
        });
        LinearLayout log = section(logPage, "连接与采样日志"); logs = text("", 12, MUTED); logs.setTextIsSelectable(true); log.addView(logs);
        button(log, "复制日志", ActionStyle.TEXT, () -> {
            android.content.ClipboardManager clipboard = (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            clipboard.setPrimaryClip(ClipData.newPlainText("AvA Tool 连接日志", logText.toString()));
            Toast.makeText(this, "日志已复制", Toast.LENGTH_SHORT).show();
        });
        button(log, "复制设备诊断", ActionStyle.TONAL, () -> {
            final String sessionLog = logText.toString();
            Toast.makeText(this, "正在读取本机诊断，不访问 USB", Toast.LENGTH_SHORT).show();
            diagnosticWorker.execute(() -> {
                String report = "AvA Tool " + appVersion() + "\n型号=" + Build.MANUFACTURER + " / " + Build.MODEL
                    + "\nAndroid=" + Build.VERSION.RELEASE + " API=" + Build.VERSION.SDK_INT
                    + "\nROM=" + Build.DISPLAY + "\nABI=" + Arrays.toString(Build.SUPPORTED_ABIS)
                    + "\n开机时长(ms)=" + SystemClock.elapsedRealtime() + "\n" + DeviceDiagnostics.bootReasons()
                    + "\n持久连接记录：\n" + diagnostics.history() + "\n本次应用日志：\n" + sessionLog;
                ui(() -> {
                    ((android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("AvA Tool 设备诊断", report));
                    Toast.makeText(this, "设备诊断已复制；没有访问 USB", Toast.LENGTH_LONG).show();
                });
            });
        });
        log.addView(text("设备诊断无需连接 J-Link。它只复制本机报告，不上传数据。", 12, MUTED));
        log.removeView(logs); log.addView(logs);
        logPage.addView(text("AvA Tool " + appVersion() + "\nADIv5 AHB-AP / Cortex-M · 未包含 Flash 烧录与 RTT", 12, MUTED));
        loadWatches(); rebuildWatches(); started = SystemClock.elapsedRealtimeNanos();
        IntentFilter filter = new IntentFilter(USB_PERMISSION); filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(Intent.ACTION_CONFIGURATION_CHANGED); filter.addAction(Intent.ACTION_TIME_CHANGED); filter.addAction(Intent.ACTION_TIMEZONE_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED); else registerReceiver(receiver, filter);
        appendLog("AvA Tool " + appVersion());
        progress.setIndeterminateTintList(ColorStateList.valueOf(ACCENT));
        refreshControls();
    }
    private String appVersion() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (android.content.pm.PackageManager.NameNotFoundException impossible) { return "Android"; }
    }
    private void ui(Runnable action) { runOnUiThread(() -> { if (!destroyed) action.run(); }); }
    private void appendLog(String value) {
        logText.append(new java.text.SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(new Date())).append("  ").append(value).append('\n');
        if (logText.length() > 6000) logText.delete(0, logText.length() - 6000);
        logs.setText(logText.toString());
    }
    private void message(String value) {
        status.setText(value); feedback.setText(value); tintText(feedback, R.color.text_secondary); feedback.setVisibility(View.VISIBLE); appendLog(value); refreshControls();
    }
    private void error(Throwable e) {
        message("错误：" + (e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage()));
        tintText(feedback, R.color.error); feedback.announceForAccessibility(feedback.getText());
    }
    private void traceConnection(String value) { diagnostics.record(value); ui(() -> appendLog(value)); }
    private void job(String title, Work work) {
        if (busy) { Toast.makeText(this, "正在执行，请稍候", Toast.LENGTH_SHORT).show(); return; }
        stopSampling(); busy = true; operation = title; feedback.setVisibility(View.GONE); appendLog(title + "…"); refreshControls();
        worker.execute(() -> {
            try { diagnostics.record(title + " 开始"); work.run(); }
            catch (Exception | LinkageError e) {
                diagnostics.record(title + " 失败：" + e);
                // Peer protocol errors (e.g. a denied lease) do not disconnect
                // an otherwise healthy peer or the independent local USB link.
                if (e instanceof IOException && !remoteSelected) closeLink();
                ui(() -> error(e));
            }
            finally { ui(() -> { busy = false; operation = ""; refreshControls(); }); }
        });
    }
    private Jlink requireLink() throws IOException { if (link == null || !link.isReady()) throw new IOException("请先连接 J-Link/SWD"); return link; }
    private void closeLink() {
        if (remoteSelected) NativePeer.close();
        Jlink previous = link; link = null;
        if (previous != null) {
            diagnostics.record("USB：释放连接");
            try { previous.close(); diagnostics.record("USB：连接已释放"); }
            catch (IOException | RuntimeException e) { diagnostics.record("USB：释放失败 " + e); }
            ui(() -> { activeDevice = null; status.setText("USB 已断开，需要重新连接"); refreshControls(); });
        }
    }
    private void chooseDevice() {
        if (!idle()) return;
        try { number(speedInput, 1, 12000, "SWD 速率"); number(apInput, 0, 255, "AHB AP"); }
        catch (IllegalArgumentException e) { error(e); return; }
        stopSampling();
        ArrayList<UsbDevice> devices = new ArrayList<>();
        for (UsbDevice d : usbManager.getDeviceList().values()) if (d.getVendorId() == 0x1366) devices.add(d);
        if (devices.isEmpty()) { message("未发现 SEGGER USB 设备。请检查 OTG、USB 供电和 J-Link 连接。"); return; }
        if (devices.size() == 1) permission(devices.get(0));
        else {
            String[] names = new String[devices.size()];
            for (int i = 0; i < names.length; i++) names[i] = devices.get(i).getProductName() + " · " + devices.get(i).getDeviceName();
            new AlertDialog.Builder(appearanceContext()).setTitle("选择 J-Link").setItems(names, (d, which) -> permission(devices.get(which))).show();
        }
    }
    private void permission(UsbDevice device) {
        if (usbManager.hasPermission(device)) { connect(device); return; }
        Intent intent = new Intent(USB_PERMISSION).setPackage(getPackageName());
        int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
        permissionPending = true; refreshControls();
        try { usbManager.requestPermission(device, PendingIntent.getBroadcast(this, 0, intent, flags)); }
        catch (RuntimeException e) { permissionPending = false; error(e); }
    }
    private int number(EditText field, int minimum, int maximum, String label) {
        try {
            int value = Integer.parseInt(field.getText().toString().trim());
            if (value < minimum || value > maximum) throw new NumberFormatException();
            field.setError(null); return value;
        } catch (NumberFormatException e) {
            String error = label + "范围：" + minimum + "–" + maximum;
            field.setError(error); field.requestFocus(); throw new IllegalArgumentException(error);
        }
    }
    private void connect(UsbDevice device) {
        if (!foreground || destroyed) return;
        final int speed, ap;
        try { speed = number(speedInput, 1, 12000, "SWD 速率"); ap = number(apInput, 0, 255, "AHB AP"); }
        catch (Exception e) { error(e); return; }
        job("连接 J-Link", () -> {
            closeLink();
            Jlink candidate = new Jlink(new AndroidUsbTransport(usbManager, device, this::traceConnection), this::traceConnection);
            try {
                candidate.connect(speed, ap); link = candidate; diagnostics.record("连接成功");
                ui(() -> { activeDevice = device; message(candidate.firmware + "\nVTref " + candidate.millivolts + " mV · SWD " + speed + " kHz\n" + String.format(Locale.ROOT, "DPIDR %08X · AP%d %08X", candidate.dpId, ap, candidate.apId)); });
            } catch (Exception e) {
                diagnostics.record("连接失败，释放 USB：" + e);
                try { candidate.close(); } catch (IOException | RuntimeException closeError) { e.addSuppressed(closeError); }
                throw e;
            }
        });
    }
    private void addDialog(String name, String address, String type) {
        stopSampling();
        LinearLayout form = column(); form.setPadding(dp(20), dp(10), dp(20), dp(8));
        EditText n = input(form, "变量名称", name, 1);
        EditText a = input(form, "地址 / 十六进制", address, 1);
        Spinner types = new Spinner(appearanceContext()); types.setAdapter(new ArrayAdapter<>(appearanceContext(), android.R.layout.simple_spinner_dropdown_item, Values.TYPES));
        types.setSelection(Arrays.asList(Values.TYPES).indexOf(type)); form.addView(types);
        ScrollView formScroll = new ScrollView(this); formScroll.addView(form);
        AlertDialog dialog = new AlertDialog.Builder(appearanceContext()).setTitle("添加监控变量").setView(formScroll).setNegativeButton("取消", null).setPositiveButton("添加", null).create();
        dialog.setOnShowListener(v -> dialog.getButton(-1).setOnClickListener(btn -> {
            try {
                if (!idle() || sampling) { Toast.makeText(this, "请先等待当前操作完成", Toast.LENGTH_SHORT).show(); return; }
                String t = types.getSelectedItem().toString(); long addr = Values.address(a.getText().toString());
                if (watches.size() >= 32) throw new IllegalArgumentException("最多监控 32 个变量");
                if (addr % Math.min(Values.size(t), 4) != 0 || addr + Values.size(t) > 0x100000000L) throw new IllegalArgumentException("地址未对齐或超出 32 位范围");
                String label = n.getText().toString().trim(); if (label.isEmpty()) label = String.format(Locale.ROOT, "0x%08X", addr);
                Watch w = new Watch(label, addr, t); watches.add(w); plot.selected = w; rebuildWatches(); saveWatches(); dialog.dismiss();
            } catch (Exception e) { a.setError(e.getMessage()); a.requestFocus(); }
        })); dialog.show();
    }
    private void rebuildWatches() {
        channels.removeAllViews();
        if (plot.selected == null && !watches.isEmpty()) plot.selected = watches.get(0);
        if (watches.isEmpty()) {
            TextView empty = text("还没有变量\n导入固件符号，或手动添加地址。", 15, MUTED);
            empty.setPadding(dp(8), dp(24), dp(8), dp(24)); channels.addView(empty);
        }
        for (Watch w : watches) {
            LinearLayout card = column(); w.cardView = card; card.setPadding(dp(12), dp(8), dp(8), dp(12));
            LinearLayout header = new LinearLayout(this); header.setOrientation(LinearLayout.HORIZONTAL); header.setGravity(Gravity.CENTER_VERTICAL);
            TextView label = text(w.name, 16, TEXT); label.setTypeface(null, Typeface.BOLD); header.addView(label, new LinearLayout.LayoutParams(0, -2, 1));
            ImageButton more = new ImageButton(appearanceContext()); more.setImageResource(R.drawable.ic_more); more.setImageTintList(ColorStateList.valueOf(MUTED));
            more.setContentDescription(w.name + "，更多操作"); moreButtons.put(more, true); tintMoreButton(more);
            header.addView(more, new LinearLayout.LayoutParams(dp(48), dp(48))); card.addView(header);
            card.addView(text(String.format(Locale.ROOT, "%s  ·  0x%08X", w.type, w.address), 12, MUTED));
            CharSequence previous = w.valueView == null ? "等待首次读取" : w.valueView.getText();
            w.valueView = text(previous.toString(), 22, ACCENT); w.valueView.setTypeface(Typeface.MONOSPACE); w.valueView.setPadding(0, dp(8), 0, 0); card.addView(w.valueView);
            LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(-1, -2); cardParams.setMargins(0, dp(10), 0, 0); channels.addView(card, cardParams);
            card.setFocusable(true); card.setMinimumHeight(dp(48));
            card.setContentDescription(w.name + "，" + w.type + "，点击查看曲线");
            card.setOnClickListener(v -> { plot.selected = w; updateWatchSelection(); plot.invalidate(); selectPage(2); refreshControls(); });
            more.setOnClickListener(v -> watchMenu(w, more));
            card.setOnLongClickListener(v -> { watchMenu(w, more); return true; });
        }
        countLabel.setText(watches.size() + " / 32 个变量 · " + csvRows.size() + " 条记录"); updateWatchSelection(); plot.invalidate(); refreshControls();
    }
    private void updateWatchSelection() {
        for (Watch w : watches) if (w.cardView != null) {
            GradientDrawable surface = background(BG); surface.setCornerRadius(dp(12));
            surface.setStroke(dp(w == plot.selected ? 2 : 1), w == plot.selected ? ACCENT : OUTLINE);
            w.cardView.setBackground(new RippleDrawable(ColorStateList.valueOf(withAlpha(ACCENT, 32)), surface, null));
            w.cardView.setSelected(w == plot.selected);
        }
    }
    private void watchMenu(Watch w, View anchor) {
        PopupMenu menu = new PopupMenu(appearanceContext(), anchor);
        menu.getMenu().add(0, 1, 0, "查看曲线");
        menu.getMenu().add(0, 2, 1, "写入数值…").setEnabled(idle() && connected() && !sampling);
        menu.getMenu().add(0, 3, 2, "移除变量…").setEnabled(idle() && !sampling);
        menu.getMenu().add(0, 4, 3, "复制名称与地址");
        menu.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == 1) { plot.selected = w; updateWatchSelection(); plot.invalidate(); selectPage(2); refreshControls(); }
            else if (item.getItemId() == 4) {
                ((android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("监控变量", w.name + String.format(Locale.ROOT, "\n0x%08X · %s", w.address, w.type)));
                Toast.makeText(this, "变量信息已复制", Toast.LENGTH_SHORT).show();
            }
            else if (item.getItemId() == 2 && idle() && connected() && !sampling) writeDialog(w);
            else if (item.getItemId() == 3 && idle() && !sampling) {
                new AlertDialog.Builder(appearanceContext()).setTitle("移除 " + w.name + "？").setMessage("仅移除监控项，不修改目标设备。已经采集的 CSV 记录保留。")
                    .setNegativeButton("取消", null).setPositiveButton("移除", (d, which) -> {
                        if (!idle() || sampling) return;
                        watches.remove(w); if (plot.selected == w) plot.selected = watches.isEmpty() ? null : watches.get(0);
                        rebuildWatches(); saveWatches();
                    }).show();
            }
            return true;
        }); menu.show();
    }
    private long readWord(long address, int size) throws IOException {
        if (remoteSelected) { byte[] bytes=NativePeer.read(address,size); long value=0; for(int i=0;i<bytes.length;i++)value|=(bytes[i]&255L)<<(8*i); return value; }
        if (PeerService.running) return PeerService.readMemory(address,size);
        return requireLink().readMemory(address,size);
    }
    private void writeWord(long address,int size,long value) throws IOException {
        if (remoteSelected || PeerService.running) { byte[] bytes=new byte[size];for(int i=0;i<size;i++)bytes[i]=(byte)(value>>(8*i));NativePeer.write(address,bytes,!remoteSelected); }
        else requireLink().writeMemory(address,size,value);
    }
    private void resetSelected() throws IOException {
        if (remoteSelected) NativePeer.reset(false);
        else if (PeerService.running) NativePeer.reset(true);
        else {requireLink().resetCortexM();closeLink();}
    }
    private void drawPeerControls(LinearLayout parent, LinearLayout localParent) {
        LinearLayout card=section(parent,"连接其他 ava_tool");
        LinearLayout actions=row();card.addView(actions);
        action(actions,"连接远端",ActionStyle.TONAL,()->idle()&&!sampling,()->peerConnectDialog("",45991));
        action(actions,"发现设备",ActionStyle.TEXT,()->idle()&&!sampling,()->job("发现局域网设备",()->{
            JSONArray peers=new JSONArray(NativePeer.discover());
            ui(()->{if(peers.length()==0){message("未发现共享设备，可手动输入 IP 地址");return;}
                String[] labels=new String[peers.length()];for(int i=0;i<labels.length;i++){JSONObject peer=peers.optJSONObject(i);labels[i]=peer.optString("name")+" · "+peer.optString("host");}
                new AlertDialog.Builder(appearanceContext()).setTitle("共享设备").setItems(labels,(dialog,which)->{JSONObject peer=peers.optJSONObject(which);peerConnectDialog(peer.optString("host"),peer.optInt("port",45991));}).setNegativeButton("取消",null).show();
            });
        }));
        action(card,"使用本机设备",ActionStyle.TEXT,()->idle()&&!sampling&&remoteSelected,()->job("切回本机",()->{NativePeer.close();remoteSelected=false;ui(()->message("已切回本机设备"));}));
        action(card,"申请 / 释放控制权",ActionStyle.TONAL,()->idle()&&remoteSelected&&NativePeer.connected(),()->job("控制权",()->{boolean release=NativePeer.ownsControl();NativePeer.claim(!release);ui(()->message(release?"已释放控制权":"已取得远端写入控制权"));}));
        action(card,"复位远端 MCU…",ActionStyle.DANGER,()->idle()&&!sampling&&remoteSelected&&NativePeer.connected()&&NativePeer.ownsControl(),()->new AlertDialog.Builder(appearanceContext()).setTitle("复位远端 MCU？").setMessage("目标板将复位，远端连接会失效，需要重新连接。").setNegativeButton("取消",null).setPositiveButton("复位",(dialog,which)->job("复位远端",()->{NativePeer.reset(false);ui(()->message("已请求远端复位，请重新连接"));})).show());
        LinearLayout shared=section(localParent,"共享本机设备");
        peerStatus=text(PeerService.status,13,MUTED);shared.addView(peerStatus);
        shareButton=action(shared,"共享本机 J-Link",ActionStyle.TONAL,()->idle()&&!sampling&&(PeerService.running||activeDevice!=null),this::shareLocalDevice);
        button(shared,"复制本机配对码",ActionStyle.TEXT,()->{
            if(PeerService.pairingKey.isEmpty()){message("请先开启本机共享");return;}
            ((android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("AvA 配对码",PeerService.pairingKey));Toast.makeText(this,"配对码已复制",Toast.LENGTH_SHORT).show();
        });
        card.addView(text("局域网 · 消息身份校验 · 数据未加密\n远端采样最高 50 Hz；暂不支持远端烧录、RTT、HSS。",12,MUTED));
    }
    private void peerConnectDialog(String host,int port) {
        LinearLayout form=column();form.setPadding(dp(20),dp(12),dp(20),dp(8));
        EditText address=input(form,"IP 地址",host,1),number=input(form,"端口",Integer.toString(port),2),key=input(form,"对方的配对码","",1);
        ScrollView scroll=new ScrollView(this);scroll.addView(form);
        AlertDialog dialog=new AlertDialog.Builder(appearanceContext()).setTitle("连接远端设备").setView(scroll).setNegativeButton("取消",null).setPositiveButton("连接",null).create();
        dialog.setOnShowListener(v->dialog.getButton(-1).setOnClickListener(button->{
            final int selectedPort;try{selectedPort=number(number,1,65535,"端口");}catch(Exception error){return;}
            String selectedHost=address.getText().toString().trim(),code=key.getText().toString().trim();
            job("连接远端",()->{NativePeer.connect(selectedHost,selectedPort,code);remoteSelected=true;ui(()->message("远端已连接，可在变量页添加地址或导入 ELF"));});dialog.dismiss();
        }));dialog.show();
    }
    private void shareLocalDevice() {
        if(PeerService.running){stopService(new Intent(this,PeerService.class));return;}
        CheckBox writes=new CheckBox(appearanceContext());writes.setText("允许远端申请写入控制权");writes.setPadding(dp(20),dp(8),dp(20),dp(8));
        new AlertDialog.Builder(appearanceContext()).setTitle("共享本机 J-Link").setMessage("开启后，锁屏或离开应用仍会共享本机设备。可随时在设备页停止共享。").setView(writes).setNegativeButton("取消",null).setPositiveButton("开启共享",(d,w)->{
            if(Build.VERSION.SDK_INT>=33&&checkSelfPermission("android.permission.POST_NOTIFICATIONS")!=android.content.pm.PackageManager.PERMISSION_GRANTED)requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"},51);
            final boolean allowWrites=writes.isChecked();
            job("开启本机共享",()->{
                Jlink local=link;if(local==null||!local.isReady())throw new IOException("请先连接本机 J-Link");String code=NativePeer.key();
                PeerService.adopt(local,activeDevice);link=null;
                ui(()->{try{startForegroundService(new Intent(this,PeerService.class).putExtra("key",code).putExtra("writes",allowWrites));message("共享正在启动，可复制配对码供另一台设备连接");}
                    catch(RuntimeException e){worker.execute(PeerService::abandon);error(e);}});
            });
        }).show();
    }
    private void startRemoteSampling() {
        if(!canRead()){message("请先连接远端并添加变量");return;}
        final int period;try{period=number(periodInput,20,60000,"采样间隔");}catch(Exception e){error(e);return;}
        ArrayList<Watch> snapshot=new ArrayList<>(watches);final JSONArray blocks=new JSONArray();
        try{for(Watch w:snapshot)blocks.put(new JSONObject().put("address",w.address).put("size",Values.size(w.type)));}catch(JSONException e){error(e);return;}
        sampling=true;remoteSampling=true;final int gen=++generation;refreshControls();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        worker.execute(()->{
            try{
                int frameSize=NativePeer.startSamples(blocks.toString(),period*1000);
                final long[] clock={-1,0};
                ui(()->{if(!sampling||gen!=generation)return;
                    poller=worker.scheduleWithFixedDelay(()->{
                        if(!sampling||gen!=generation)return;
                        try{byte[] data=NativePeer.samples();if(frameSize<5||data.length%frameSize!=0)throw new IOException("远端采样帧不完整");
                            ArrayList<long[]> values=new ArrayList<>();ArrayList<Double> times=new ArrayList<>();
                            for(int at=0;at<data.length;at+=frameSize){long stamp=Jlink.u32(data,at);if(clock[0]>=0)clock[1]+=(stamp-clock[0])&0xffffffffL;clock[0]=stamp;times.add(clock[1]/1e6);long[] row=new long[snapshot.size()];int offset=at+4;
                                for(int i=0;i<snapshot.size();i++){int size=Values.size(snapshot.get(i).type);for(int n=0;n<size;n++)row[i]|=(data[offset+n]&255L)<<(8*n);offset+=size;}values.add(row);}
                            ui(()->{if(gen!=generation)return;for(int row=0;row<values.size();row++)for(int i=0;i<snapshot.size();i++)if(watches.contains(snapshot.get(i)))record(snapshot.get(i),values.get(row)[i],times.get(row));});
                        }catch(Exception e){ui(()->{if(gen==generation){stopSampling();error(e);}});}
                    },0,50,TimeUnit.MILLISECONDS);
                });
            }catch(Exception e){ui(()->{if(gen==generation){stopSampling();error(e);}});}
        });
    }
    private long read(Watch w) throws IOException {
        int size = Values.size(w.type);
        long value = readWord(w.address, Math.min(4, size));
        if (size == 8) value |= readWord(w.address + 4, 4) << 32;
        return value;
    }
    private void record(Watch w, long bits, double time) {
        w.valueView.setText(Values.format(w.type, bits));
        w.history.addLast(new Sample(time, Values.numeric(w.type, bits))); if (w.history.size() > 1200) w.history.removeFirst();
        csvRows.addLast(String.format(Locale.ROOT, "%.6f,", time) + Values.csv(w.name) + String.format(Locale.ROOT, ",0x%08X,", w.address) + w.type + "," + Values.format(w.type, bits) + "\n");
        if (csvRows.size() > 10000) csvRows.removeFirst();
        countLabel.setText(watches.size() + " 个变量 · " + csvRows.size() + " 条记录（最多 10000）"); plot.invalidate(); refreshScopeValues(false);
        if (!sampling) refreshControls();
    }
    private void readOnce() {
        if (watches.isEmpty()) { message("请先添加变量"); return; }
        ArrayList<Watch> snapshot = new ArrayList<>(watches);
        job("读取变量", () -> {
            int gen = generation;
            for (Watch w : snapshot) { long bits = read(w); double time = (SystemClock.elapsedRealtimeNanos() - started) / 1e9;
                ui(() -> { if (gen == generation && watches.contains(w)) record(w, bits, time); }); }
        });
    }
    private void startSampling() {
        if (remoteSelected) { startRemoteSampling(); return; }
        if (!canRead()) { message("请先连接设备、添加变量，并等待当前操作完成。"); return; }
        if (watches.isEmpty()) { message("请先添加变量"); return; }
        final int period;
        try { period = number(periodInput, 20, 60000, "采样间隔"); }
        catch (Exception e) { error(e); return; }
        sampling = true; int gen = ++generation;
        worker.execute(() -> diagnostics.record("采样开始，间隔=" + period + " ms"));
        ArrayList<Watch> snapshot = new ArrayList<>(watches);
        poller = worker.scheduleWithFixedDelay(() -> {
            if (!sampling) return;
            try {
                for (Watch w : snapshot) {
                    if (!sampling) return;
                    long bits = read(w); double time = (SystemClock.elapsedRealtimeNanos() - started) / 1e9;
                    ui(() -> { if (gen == generation && watches.contains(w)) record(w, bits, time); });
                }
            } catch (Exception e) {
                sampling = false; diagnostics.record("采样失败：" + e); closeLink();
                ui(() -> { if (gen == generation) { stopSampling(); error(e); } });
            }
        }, 0, period, TimeUnit.MILLISECONDS);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        appendLog("开始采样：每轮完成后等待 " + period + " ms，按实际读取时刻记录");
        feedback.setVisibility(View.GONE); refreshControls();
    }
    private void stopSampling() {
        sampling = false; generation++;
        if (remoteSampling) { remoteSampling = false; worker.execute(NativePeer::stopSamples); }
        if (poller != null) { poller.cancel(false); poller = null; }
        if (hssSampling) {
            hssSampling=false;
            activeDevice = null;
            HssCapture capture=hssCapture; Watch watch=hssWatch;
            worker.execute(() -> {
                closeLink(); // stops probe acquisition even when Activity is backgrounded
                ui(() -> { if (!sampling && capture == hssCapture) showHss(capture,watch); });
            });
        }
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        refreshControls();
    }
    private void startHssSampling() {
        if (remoteSelected || PeerService.running) { message("远端与共享模式使用普通采样，HSS 实验功能需独占本机 USB。"); return; }
        if (!canRead()) { message("请先连接设备、添加变量，并等待当前操作完成。"); return; }
        final Watch watch=plot.selected;
        if (watch==null || Values.size(watch.type)!=4) { message("请选择一个 32 位 RAM 变量（f32 / i32 / u32）"); return; }
        final int hz;
        try {
            hz=number(hssHzInput, 1, 50000, "HSS 目标频率"); JlinkHss.periodFor(hz);
            if (!JlinkHss.supportedRam(watch.address)) throw new IOException("HSS 地址必须是 H74x 片内 RAM 中对齐的 32 位变量");
        } catch (Exception e) { error(e); return; }
        final HssCapture capture;
        try { capture=new HssCapture(JlinkHss.periodFor(hz)); } catch (IOException e) { error(e); return; }
        hssCapture=capture; hssWatch=watch; hssSampling=true; sampling=true; int gen=++generation;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        appendLog("HSS 启动："+watch.name+"，目标 "+hz+" Hz；频率以探针时间戳实测为准");
        feedback.setVisibility(View.GONE); refreshControls();
        worker.execute(() -> {
            if (!sampling || gen!=generation) return;
            try {
                JlinkHss session=requireLink().startHss(watch.address,hz);
                diagnostics.record("HSS 开始：目标="+hz+" Hz，地址="+Long.toHexString(watch.address));
                final long[] lastUi={0};
                // Schedule assignment is on the UI thread: stop cannot race with publishing poller.
                ui(() -> {
                    if (!sampling || gen!=generation) return;
                    poller=worker.scheduleWithFixedDelay(() -> {
                        if (!sampling || gen!=generation) return;
                        try {
                            capture.accept(session.read());
                            long now=SystemClock.elapsedRealtime();
                            if (now-lastUi[0]>=100 && hssUiPending.compareAndSet(false,true)) {
                                lastUi[0]=now;
                                ui(() -> {
                                    try { if (gen==generation) showHss(capture,watch); }
                                    finally { hssUiPending.set(false); }
                                });
                            }
                        } catch (Exception e) { hssFailure(gen,e); }
                    },0,2,TimeUnit.MILLISECONDS);
                });
            } catch (Exception e) { hssFailure(gen,e); }
        });
    }
    private void hssFailure(int gen, Exception e) {
        diagnostics.record("HSS 失败："+e); closeLink();
        ui(() -> { if (gen==generation) { stopSampling(); error(e); } });
    }
    private void showHss(HssCapture capture, Watch watch) {
        if (capture==null || watch==null) return;
        HssCapture.Snapshot frame=capture.snapshot(1200);
        if (watches.contains(watch)) {
            watch.history.clear();
            for (int i=0;i<frame.values.length;i++) watch.history.addLast(new Sample(frame.times[i]/1e6,Values.numeric(watch.type,frame.values[i]&0xffffffffL)));
            if (frame.values.length>0) watch.valueView.setText(Values.format(watch.type,frame.values[frame.values.length-1]&0xffffffffL));
        }
        countLabel.setText(String.format(Locale.ROOT,"HSS %s · 实测 %.1f Hz\n累计 %d · 保留 %d · 覆盖 %d · 长间隔 %d",watch.name,frame.hz,frame.total,frame.retained,frame.total-frame.retained,frame.gaps));
        if (scopeSummary != null && plot.selected == watch) scopeSummary.setText(String.format(Locale.ROOT, "HSS · %.1f Hz · %d 个显示点", frame.hz, frame.values.length));
        plot.invalidate(); refreshScopeValues(true);
    }
    private void exportHssCsv() {
        if (busy || hssCapture==null) { message("请先完成 HSS 采样"); return; }
        stopSampling(); final HssCapture capture=hssCapture; final Watch watch=hssWatch;
        job("生成 HSS CSV", () -> {
            HssCapture.Snapshot data=capture.snapshot(HssCapture.CAPACITY);
            if (data.values.length==0) throw new IOException("HSS 没有采样点");
            String prefix=Values.csv(watch.name)+String.format(Locale.ROOT,",0x%08X,",watch.address)+watch.type+",";
            StringBuilder csv=new StringBuilder("probe_elapsed_us,name,address,type,value\n");
            for(int i=0;i<data.values.length;i++) csv.append(data.times[i]).append(',').append(prefix).append(Values.format(watch.type,data.values[i]&0xffffffffL)).append('\n');
            ui(() -> { pendingCsv=csv.toString(); startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("text/csv").putExtra(Intent.EXTRA_TITLE,"ava_hss_capture.csv"),11); });
        });
    }
    private void writeDialog(Watch w) {
        stopSampling();
        LinearLayout form = column(); form.setPadding(dp(20), 0, dp(20), 0);
        form.addView(text("直接写目标 RAM/寄存器并回读。请确认地址和类型。\n64 位值会分两次写入，不保证原子性。", 13, MUTED));
        EditText value = input(form, w.name + " / " + w.type, "", 1);
        ScrollView formScroll = new ScrollView(this); formScroll.addView(form);
        AlertDialog dialog = new AlertDialog.Builder(appearanceContext()).setTitle("写入变量").setView(formScroll).setNegativeButton("取消", null).setPositiveButton("写入并校验", null).create();
        dialog.setOnShowListener(v -> dialog.getButton(-1).setOnClickListener(btn -> {
            try {
                final long bits = Values.parse(w.type, value.getText().toString());
                if (busy) { Toast.makeText(this, "请等待当前操作完成", Toast.LENGTH_SHORT).show(); return; }
                job("写入 " + w.name, () -> {
                    int size = Values.size(w.type);
                    writeWord(w.address, Math.min(size, 4), bits);
                    if (size == 8) writeWord(w.address + 4, 4, bits >>> 32);
                    long actual = read(w);
                    long mask = size == 8 ? -1L : (1L << (size * 8)) - 1;
                    if ((actual & mask) != (bits & mask)) throw new IOException("回读不一致：" + Values.format(w.type, actual));
                    double time = (SystemClock.elapsedRealtimeNanos() - started) / 1e9;
                    ui(() -> { if (watches.contains(w)) record(w, actual, time); appendLog(w.name + " 写入及回读校验通过"); });
                }); dialog.dismiss();
            } catch (Exception e) { value.setError(e.getMessage()); value.requestFocus(); }
        })); dialog.show();
    }
    private void showSymbols() {
        if (symbols.length() == 0) { message("尚未导入符号，或文件中没有可监控的标量"); return; }
        LinearLayout content = column(); EditText search = input(content, "搜索变量（最多显示 300 条）", "", 1);
        TextView resultCount = text("", 12, MUTED); resultCount.setPadding(dp(12), dp(4), dp(12), dp(4)); content.addView(resultCount);
        int listHeight = Math.max(100, Math.min(350, getResources().getConfiguration().screenHeightDp - (int)(220 * getResources().getConfiguration().fontScale)));
        ListView list = new ListView(appearanceContext()); content.addView(list, new LinearLayout.LayoutParams(-1, dp(listHeight)));
        ArrayList<JSONObject> filtered = new ArrayList<>();
        ArrayAdapter<String> adapter = new ArrayAdapter<>(appearanceContext(), android.R.layout.simple_list_item_1, new ArrayList<>()); list.setAdapter(adapter);
        Runnable refresh = () -> {
            adapter.clear(); filtered.clear(); String query = search.getText().toString().toLowerCase(Locale.ROOT);
            for (int i = 0; i < symbols.length() && filtered.size() < 300; i++) {
                JSONObject s = symbols.optJSONObject(i);
                if (s != null && s.optString("name").toLowerCase(Locale.ROOT).contains(query)) {
                    filtered.add(s); adapter.add(s.optString("name") + "  (" + s.optString("type") + ")\n" + String.format(Locale.ROOT, "0x%08X", s.optLong("address")));
                }
            }
            resultCount.setText(filtered.isEmpty() ? "没有匹配的变量，请尝试名称中的其他文字。" : filtered.size() + " 个匹配" + (filtered.size() == 300 ? "（仅显示前 300 项，请继续缩小搜索）" : ""));
        };
        search.addTextChangedListener(new TextWatcher() { public void beforeTextChanged(CharSequence s, int st, int c, int a) {} public void onTextChanged(CharSequence s, int st, int b, int c) { refresh.run(); } public void afterTextChanged(Editable e) {} });
        refresh.run();
        AlertDialog dialog = new AlertDialog.Builder(appearanceContext()).setTitle("ELF / AXF 符号").setView(content).setNegativeButton("关闭", null).create();
        list.setOnItemClickListener((p, v, index, id) -> { JSONObject s = filtered.get(index); dialog.dismiss(); addDialog(s.optString("name"), Long.toHexString(s.optLong("address")), s.optString("type")); }); dialog.show();
    }
    private void exportCsv() {
        stopSampling(); if (csvRows.isEmpty()) { message("没有可导出的记录"); return; }
        StringBuilder b = new StringBuilder("elapsed_s,name,address,type,value\n"); for (String row : csvRows) b.append(row); pendingCsv = b.toString();
        startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("text/csv").putExtra(Intent.EXTRA_TITLE, "ava_capture.csv"), 11);
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data); if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (request == 10) job("解析 ELF / AXF", () -> {
            File file = new File(getCacheDir(), "import.elf");
            try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(file)) {
                if (in == null) throw new IOException("无法打开文件");
                byte[] buffer = new byte[32768]; int total = 0, n;
                while ((n = in.read(buffer)) != -1) { total += n; if (total > 64 * 1024 * 1024) throw new IOException("文件超过 64 MiB"); out.write(buffer, 0, n); }
            }
            try {
                JSONObject parsed = new JSONObject(NativeCore.parseElf(file.getAbsolutePath())); JSONArray loaded = parsed.getJSONArray("symbols");
                ui(() -> { symbols = loaded; appendLog("已载入 " + loaded.length() + " 个标量符号。结构体展开，单维数组最多 256 项；无类型信息的符号按无符号整数显示，请核对。" + (parsed.optBoolean("truncated") ? " 已达 20000 项上限。" : "")); showSymbols(); });
            } finally { file.delete(); }
        });
        if (request == 11 && pendingCsv != null) {
            String value = pendingCsv; pendingCsv = null;
            job("导出 CSV", () -> { try (OutputStream out = getContentResolver().openOutputStream(uri, "wt")) { if (out == null) throw new IOException("无法写入文件"); out.write(value.getBytes(StandardCharsets.UTF_8)); } ui(() -> appendLog("CSV 已保存")); });
        }
    }
    private void saveWatches() {
        JSONArray data = new JSONArray();
        try { for (Watch w : watches) { JSONObject v = new JSONObject(); v.put("name", w.name); v.put("address", w.address); v.put("type", w.type); data.put(v); } } catch (JSONException ignored) {}
        getPreferences(MODE_PRIVATE).edit().putString("watches", data.toString()).putString("speed", speedInput.getText().toString()).putString("ap", apInput.getText().toString()).putString("period", periodInput.getText().toString()).putString("hssHz", hssHzInput.getText().toString()).apply();
    }
    private void loadWatches() {
        try { JSONArray data = new JSONArray(getPreferences(MODE_PRIVATE).getString("watches", "[]"));
            for (int i = 0; i < data.length() && i < 32; i++) { JSONObject v = data.getJSONObject(i); String type = v.getString("type"); Values.size(type); watches.add(new Watch(v.getString("name"), v.getLong("address"), type)); }
        } catch (Exception e) { appendLog("无法恢复部分变量：" + e.getMessage()); }
    }
    @Override protected void onPause() { stopSampling(); saveWatches(); super.onPause(); }
    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config); applyAppearance(true);
    }
    @Override protected void onStart() {
        super.onStart(); foreground = true; peerHandler.removeCallbacks(peerTick); peerTick.run(); refreshControls(); appearanceHandler.removeCallbacks(appearanceTick); appearanceTick.run();
    }
    @Override protected void onStop() {
        foreground = false; peerHandler.removeCallbacks(peerTick); appearanceHandler.removeCallbacks(appearanceTick); stopSampling(); worker.execute(this::closeLink); super.onStop();
    }
    @Override protected void onDestroy() {
        destroyed = true; peerHandler.removeCallbacks(peerTick); appearanceHandler.removeCallbacks(appearanceTick); stopSampling(); unregisterReceiver(receiver);
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null) getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        worker.execute(this::closeLink); worker.shutdown(); diagnosticWorker.shutdown(); super.onDestroy();
    }
    final class Plot extends View {
        Watch selected; final Paint paint = new Paint(3);
        Plot() { super(MainActivity.this); }
        @Override protected void onDraw(Canvas c) {
            super.onDraw(c);
            paint.setTextSize(11 * getResources().getDisplayMetrics().scaledDensity); paint.setColor(MUTED);
            if (selected == null || selected.history.size() < 2) {
                String hint = selected == null ? "等待添加监控变量" : "等待采样数据";
                paint.setTextAlign(Paint.Align.CENTER); c.drawText(hint, getWidth() / 2f, getHeight() / 2f, paint); paint.setTextAlign(Paint.Align.LEFT);
                return;
            }
            double min = Double.POSITIVE_INFINITY, max = Double.NEGATIVE_INFINITY;
            for (Sample s : selected.history) if (Double.isFinite(s.value)) { min = Math.min(min, s.value); max = Math.max(max, s.value); }
            if (!Double.isFinite(min)) return;
            if (max == min) { double pad = Math.max(Math.abs(min) * .01, .001); max += pad; min -= pad; }
            String[] labels = new String[5]; float labelWidth = 0;
            for (int i = 0; i < labels.length; i++) {
                labels[i] = String.format(Locale.ROOT, "%.3g", max - (max - min) * i / 4);
                labelWidth = Math.max(labelWidth, paint.measureText(labels[i]));
            }
            float lineHeight = paint.getFontSpacing();
            float left = labelWidth + dp(10), top = lineHeight, right = getWidth() - dp(8), bottom = getHeight() - lineHeight * 2 - dp(8);
            if (right <= left || bottom <= top) return;
            double start = selected.history.getFirst().t, end = selected.history.getLast().t;
            if (end <= start) end = start + .001;
            for (int i = 0; i < labels.length; i++) {
                float y = top + (bottom - top) * i / 4; paint.setColor(OUTLINE); c.drawLine(left, y, right, y, paint);
                paint.setColor(MUTED); c.drawText(labels[i], 0, y + lineHeight / 4, paint);
            }
            String beginLabel = String.format(Locale.ROOT, "%.2f s", start), endLabel = String.format(Locale.ROOT, "%.2f s", end);
            float beginWidth = paint.measureText(beginLabel), endWidth = paint.measureText(endLabel);
            c.drawText(beginLabel, left, bottom + lineHeight, paint);
            // Use a second baseline at large font sizes rather than overlap time labels.
            c.drawText(endLabel, Math.max(left, right - endWidth), bottom + lineHeight * (beginWidth + endWidth + dp(12) > right - left ? 2 : 1), paint);
            Path path = new Path(); boolean first = true;
            for (Sample s : selected.history) { if (!Double.isFinite(s.value)) { first = true; continue; } float x = left + (float)((s.t - start) / (end - start)) * (right - left); float y = bottom - (float)((s.value - min) / (max - min)) * (bottom - top); if (first) path.moveTo(x, y); else path.lineTo(x, y); first = false; }
            paint.setColor(ACCENT); paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(1.5f)); c.drawPath(path, paint); paint.setStyle(Paint.Style.FILL);
        }
    }
}
