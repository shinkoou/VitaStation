package org.vita3k.emulator.ui.screens.emulation;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.BatteryManager;
import android.os.Debug;
import android.os.Process;
import android.view.View;

import org.vita3k.emulator.NativeLib;
import org.vita3k.emulator.data.PerformanceHudPrefs;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class PerformanceHudView extends View {
    private final Paint backgroundPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint labelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint valuePaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final float density;
    private int fps;
    private float ramMb;
    private float cpuPercent;
    private Float gpuPercent;
    private Float batteryTemp;
    private long previousCpuMs;
    private long previousWallMs;

    private final Runnable updater = new Runnable() {
        @Override
        public void run() {
            updateMetrics();
            invalidate();
            postDelayed(this, 1000L);
        }
    };

    public PerformanceHudView(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        setWillNotDraw(false);
        setClickable(false);
        setFocusable(false);

        backgroundPaint.setColor(Color.argb(224, 8, 13, 30));
        borderPaint.setColor(Color.rgb(89, 216, 255));
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(dp(1.2f));

        labelPaint.setColor(Color.rgb(149, 163, 188));
        labelPaint.setTypeface(android.graphics.Typeface.create(
                "sans-serif-medium", android.graphics.Typeface.NORMAL));
        valuePaint.setColor(Color.WHITE);
        valuePaint.setTypeface(android.graphics.Typeface.create(
                "sans-serif", android.graphics.Typeface.BOLD));

        previousCpuMs = Process.getElapsedCpuTime();
        previousWallMs = android.os.SystemClock.elapsedRealtime();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        removeCallbacks(updater);
        post(updater);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(updater);
        super.onDetachedFromWindow();
    }

    private void updateMetrics() {
        boolean enabled = PerformanceHudPrefs.isMasterEnabled(getContext());
        setVisibility(enabled ? VISIBLE : GONE);
        if (!enabled) return;

        try {
            fps = NativeLib.INSTANCE.getCurrentFps();
        } catch (Throwable ignored) {
            fps = 0;
        }

        ramMb = Debug.getPss() / 1024f;

        long nowCpu = Process.getElapsedCpuTime();
        long nowWall = android.os.SystemClock.elapsedRealtime();
        long cpuDelta = Math.max(0L, nowCpu - previousCpuMs);
        long wallDelta = Math.max(1L, nowWall - previousWallMs);
        int cores = Math.max(1, Runtime.getRuntime().availableProcessors());
        cpuPercent = Math.min(100f, (cpuDelta * 100f) / (wallDelta * cores));
        previousCpuMs = nowCpu;
        previousWallMs = nowWall;

        gpuPercent = readGpuUsage();
        batteryTemp = readBatteryTemperature();
    }

    private Float readBatteryTemperature() {
        try {
            Intent battery = getContext().registerReceiver(
                    null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (battery == null) return null;
            int tenths = battery.getIntExtra(
                    BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
            if (tenths == Integer.MIN_VALUE || tenths <= 0) return null;
            return tenths / 10f;
        } catch (Throwable ignored) {
            return null;
        }
    }

    private Float readGpuUsage() {
        Float direct = readSinglePercent(
                "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
        if (direct != null) return direct;

        try {
            String text = readText("/sys/class/kgsl/kgsl-3d0/gpubusy");
            if (text != null) {
                String[] parts = text.trim().split("\\s+");
                if (parts.length >= 2) {
                    float busy = Float.parseFloat(parts[0]);
                    float total = Float.parseFloat(parts[1]);
                    if (total > 0f) return clamp((busy / total) * 100f);
                }
            }
        } catch (Throwable ignored) {
        }

        try {
            File devfreq = new File("/sys/class/devfreq");
            File[] devices = devfreq.listFiles();
            if (devices != null) {
                for (File device : devices) {
                    String name = device.getName().toLowerCase(Locale.ROOT);
                    if (!name.contains("gpu") && !name.contains("mali")) continue;
                    for (String leaf : new String[]{"load", "utilization", "busy_percent"}) {
                        Float value = readSinglePercent(new File(device, leaf).getAbsolutePath());
                        if (value != null) return value;
                    }
                }
            }
        } catch (Throwable ignored) {
        }

        return null;
    }

    private Float readSinglePercent(String path) {
        try {
            String text = readText(path);
            if (text == null) return null;
            String token = text.trim().split("\\s+")[0].replace("%", "");
            float value = Float.parseFloat(token);
            if (value > 100f && value <= 1000f) value /= 10f;
            if (value < 0f) return null;
            return clamp(value);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private String readText(String path) {
        try {
            File file = new File(path);
            if (!file.canRead()) return null;
            return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private float clamp(float value) {
        return Math.max(0f, Math.min(100f, value));
    }

    private float dp(float value) {
        return value * density;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (getVisibility() != VISIBLE) return;

        List<String[]> metrics = new ArrayList<>();
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_FPS)) {
            metrics.add(new String[]{"FPS", fps > 0 ? Integer.toString(fps) : "N/A"});
        }
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_RAM)) {
            metrics.add(new String[]{"RAM", ramMb >= 1024f
                    ? String.format(Locale.US, "%.1f GB", ramMb / 1024f)
                    : String.format(Locale.US, "%.0f MB", ramMb)});
        }
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_CPU)) {
            metrics.add(new String[]{"CPU", String.format(Locale.US, "%.0f%%", cpuPercent)});
        }
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_GPU)) {
            metrics.add(new String[]{"GPU", gpuPercent == null
                    ? "N/A"
                    : String.format(Locale.US, "%.0f%%", gpuPercent)});
        }
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_BATTERY)) {
            metrics.add(new String[]{"T. BATT", batteryTemp == null
                    ? "N/A"
                    : String.format(Locale.US, "%.1f°C", batteryTemp)});
        }

        if (metrics.isEmpty()) return;

        float left = dp(10f);
        float top = dp(10f);
        float right = getWidth() - dp(10f);
        float height = dp(68f);
        RectF panel = new RectF(left, top, right, top + height);

        canvas.drawRoundRect(panel, dp(18f), dp(18f), backgroundPaint);
        canvas.drawRoundRect(panel, dp(18f), dp(18f), borderPaint);

        labelPaint.setTextSize(dp(10.5f));
        valuePaint.setTextSize(dp(16f));
        labelPaint.setTextAlign(Paint.Align.CENTER);
        valuePaint.setTextAlign(Paint.Align.CENTER);

        float cellWidth = (right - left) / metrics.size();
        for (int index = 0; index < metrics.size(); index++) {
            float x = left + cellWidth * index + cellWidth / 2f;
            String[] metric = metrics.get(index);
            canvas.drawText(metric[0], x, top + dp(25f), labelPaint);
            canvas.drawText(metric[1], x, top + dp(49f), valuePaint);

            if (index > 0) {
                Paint separator = new Paint(Paint.ANTI_ALIAS_FLAG);
                separator.setColor(Color.argb(75, 139, 92, 246));
                canvas.drawRect(
                        left + cellWidth * index,
                        top + dp(16f),
                        left + cellWidth * index + dp(1f),
                        top + height - dp(16f),
                        separator);
            }
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        setMeasuredDimension(width, (int) dp(88f));
    }
}
