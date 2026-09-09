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
import android.os.SystemClock;
import android.util.Log;
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
    private static final String TAG = "VitaStationHud";
    private static final long FAST_UPDATE_MS = 500L;
    private static final long SLOW_UPDATE_MS = 2000L;
    private static final long GPU_DIAGNOSTIC_MS = 10_000L;

    private final Paint panelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint labelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint valuePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint shaderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint shaderTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final float density;
    private final boolean hudEnabled;
    private int fps;
    private boolean frameGenerationActive;
    private int frameGenerationMultiplier = 1;
    private int shaderCount;
    private float ramMb;
    private float cpuPercent;
    private Float gpuPercent;
    private Float batteryTemp;
    private long previousCpuMs;
    private long previousWallMs;
    private long lastSlowUpdateMs;
    private long lastGpuDiagnosticMs;
    private String lastGpuDiagnosticKey = "";

    private final Runnable updater = new Runnable() {
        @Override public void run() {
            if (hudEnabled) {
                updateFastMetrics();
                long now = SystemClock.elapsedRealtime();
                if (lastSlowUpdateMs == 0L || now - lastSlowUpdateMs >= SLOW_UPDATE_MS) {
                    updateSlowMetrics(now);
                }
                invalidate();
                postDelayed(this, FAST_UPDATE_MS);
            }
        }
    };

    public PerformanceHudView(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        hudEnabled = PerformanceHudPrefs.isMasterEnabled(context);
        setWillNotDraw(false);
        setClickable(false);
        setFocusable(false);
        setVisibility(hudEnabled ? VISIBLE : GONE);

        panelPaint.setColor(Color.argb(110, 7, 12, 27));
        borderPaint.setColor(Color.argb(112, 89, 216, 255));
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(dp(0.7f));

        labelPaint.setColor(Color.argb(220, 170, 181, 201));
        labelPaint.setTypeface(android.graphics.Typeface.create(
                "sans-serif-medium", android.graphics.Typeface.NORMAL));
        valuePaint.setColor(Color.WHITE);
        valuePaint.setTypeface(android.graphics.Typeface.create(
                "sans-serif", android.graphics.Typeface.BOLD));

        shaderPaint.setColor(Color.argb(112, 35, 23, 73));
        shaderTextPaint.setColor(Color.argb(235, 205, 188, 255));
        shaderTextPaint.setTypeface(android.graphics.Typeface.create(
                "sans-serif-medium", android.graphics.Typeface.NORMAL));

        previousCpuMs = Process.getElapsedCpuTime();
        previousWallMs = SystemClock.elapsedRealtime();
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        removeCallbacks(updater);
        if (hudEnabled) post(updater);
    }

    @Override protected void onDetachedFromWindow() {
        removeCallbacks(updater);
        super.onDetachedFromWindow();
    }

    private void updateFastMetrics() {
        try {
            fps = NativeLib.INSTANCE.getCurrentFps();
            frameGenerationActive = NativeLib.INSTANCE.isLsfgFrameGenerationActive();
            frameGenerationMultiplier = Math.max(1, NativeLib.INSTANCE.getLsfgFrameGenerationMultiplier());
            shaderCount = NativeLib.INSTANCE.getRecentShaderCompileCount();
        } catch (Throwable ignored) {
            fps = 0;
            frameGenerationActive = false;
            frameGenerationMultiplier = 1;
            shaderCount = 0;
        }
    }

    private void updateSlowMetrics(long nowWall) {
        ramMb = Debug.getPss() / 1024f;

        long nowCpu = Process.getElapsedCpuTime();
        long cpuDelta = Math.max(0L, nowCpu - previousCpuMs);
        long wallDelta = Math.max(1L, nowWall - previousWallMs);
        int cores = Math.max(1, Runtime.getRuntime().availableProcessors());
        cpuPercent = Math.min(100f, (cpuDelta * 100f) / (wallDelta * cores));
        previousCpuMs = nowCpu;
        previousWallMs = nowWall;

        gpuPercent = readGpuUsage();
        batteryTemp = readBatteryTemperature();
        lastSlowUpdateMs = nowWall;
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
        for (String path : new String[]{
                "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
                "/sys/class/kgsl/kgsl-3d0/devfreq/gpu_load",
                "/sys/class/kgsl/kgsl-3d0/devfreq/load",
                "/sys/class/kgsl/kgsl-3d0/devfreq/utilization",
                "/sys/class/kgsl/kgsl-3d0/devfreq/busy_percent"}) {
            Float value = readSinglePercent(path);
            if (value != null) {
                logGpuMetric(path, "OK", value);
                return value;
            }
        }

        try {
            String text = readText("/sys/class/kgsl/kgsl-3d0/gpubusy");
            if (text != null) {
                String[] parts = text.trim().split("\s+");
                if (parts.length >= 2) {
                    float busy = Float.parseFloat(parts[0]);
                    float total = Float.parseFloat(parts[1]);
                    if (total > 0f) {
                        float value = clamp((busy / total) * 100f);
                        logGpuMetric(
                                "/sys/class/kgsl/kgsl-3d0/gpubusy",
                                "OK",
                                value);
                        return value;
                    }
                }
            }
        } catch (Throwable ignored) {
        }

        try {
            File devfreq = new File("/sys/class/devfreq");
            File[] devices = devfreq.listFiles();
            if (devices != null) {
                for (File device : devices) {
                    String descriptor = device.getName().toLowerCase(Locale.ROOT);
                    try {
                        descriptor += " "
                                + device.getCanonicalPath()
                                .toLowerCase(Locale.ROOT);
                    } catch (Throwable ignored) {
                    }

                    if (!descriptor.contains("gpu")
                            && !descriptor.contains("mali")
                            && !descriptor.contains("kgsl")
                            && !descriptor.contains("3d0")) {
                        continue;
                    }

                    for (String leaf : new String[]{
                            "load",
                            "utilization",
                            "busy_percent",
                            "gpu_load"}) {
                        String path = new File(device, leaf).getAbsolutePath();
                        Float value = readSinglePercent(path);
                        if (value != null) {
                            logGpuMetric(path, "OK", value);
                            return value;
                        }
                    }
                }
            }
        } catch (Throwable ignored) {
        }

        logGpuMetric("none", "UNAVAILABLE", null);
        return null;
    }

    private void logGpuMetric(String source, String status, Float value) {
        long now = SystemClock.elapsedRealtime();
        String key = source + "|" + status;
        if (!key.equals(lastGpuDiagnosticKey)
                || now - lastGpuDiagnosticMs >= GPU_DIAGNOSTIC_MS) {
            lastGpuDiagnosticKey = key;
            lastGpuDiagnosticMs = now;
            Log.i(TAG,
                    "[VS-GPU-METRIC] source=" + source
                            + " status=" + status
                            + (value == null
                                    ? ""
                                    : " usage="
                                            + String.format(
                                                    Locale.US,
                                                    "%.1f",
                                                    value)));
        }
    }

    private Float readSinglePercent(String path) {
        try {
            String text = readText(path);
            if (text == null) return null;
            String token = text.trim().split("\\s+")[0].replace("%", "");
            int at = token.indexOf('@');
            if (at > 0) token = token.substring(0, at);
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

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (!hudEnabled || getVisibility() != VISIBLE) return;

        List<String[]> metrics = new ArrayList<>();
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_FPS)) {
            int presentedFps = frameGenerationActive && fps > 0 ? fps * frameGenerationMultiplier : fps;
            metrics.add(new String[]{"FPS", presentedFps > 0 ? Integer.toString(presentedFps) : "—"});
        }
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_RAM))
            metrics.add(new String[]{"RAM", ramMb >= 1024f
                    ? String.format(Locale.US, "%.1fG", ramMb / 1024f)
                    : String.format(Locale.US, "%.0fM", ramMb)});
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_CPU))
            metrics.add(new String[]{"CPU", String.format(Locale.US, "%.0f%%", cpuPercent)});
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_GPU))
            metrics.add(new String[]{"GPU", gpuPercent == null ? "—"
                    : String.format(Locale.US, "%.0f%%", gpuPercent)});
        if (PerformanceHudPrefs.get(getContext(), PerformanceHudPrefs.KEY_BATTERY))
            metrics.add(new String[]{"BATT", batteryTemp == null ? "—"
                    : String.format(Locale.US, "%.1f°C", batteryTemp)});

        if (metrics.isEmpty()) return;

        float available = Math.max(dp(180f), getWidth() - dp(24f));
        float panelWidth = Math.min(available, dp(500f));
        float left = (getWidth() - panelWidth) / 2f;
        float top = dp(4f);
        float height = dp(30f);
        RectF panel = new RectF(left, top, left + panelWidth, top + height);

        canvas.drawRoundRect(panel, dp(11f), dp(11f), panelPaint);
        canvas.drawRoundRect(panel, dp(11f), dp(11f), borderPaint);

        labelPaint.setTextSize(dp(7.2f));
        valuePaint.setTextSize(dp(10.6f));

        float cellWidth = panelWidth / metrics.size();
        for (int i = 0; i < metrics.size(); i++) {
            float center = left + (cellWidth * i) + (cellWidth / 2f);
            String[] metric = metrics.get(i);
            float labelWidth = labelPaint.measureText(metric[0]);
            float valueWidth = valuePaint.measureText(metric[1]);
            float gap = dp(3.5f);
            float start = center - (labelWidth + gap + valueWidth) / 2f;
            float baseline = top + dp(19.7f);

            labelPaint.setTextAlign(Paint.Align.LEFT);
            valuePaint.setTextAlign(Paint.Align.LEFT);
            canvas.drawText(metric[0], start, baseline, labelPaint);
            canvas.drawText(metric[1], start + labelWidth + gap, baseline, valuePaint);
        }

        List<String> statusPills = new ArrayList<>();
        if (frameGenerationActive && fps > 0) {
            statusPills.add("FR: " + fps + "  |  FG: " + frameGenerationMultiplier + "x");
        }
        if (shaderCount > 0) {
            statusPills.add("SHADERS  •  " + shaderCount);
        }

        if (!statusPills.isEmpty()) {
            shaderTextPaint.setTextSize(dp(8.0f));
            shaderTextPaint.setTextAlign(Paint.Align.CENTER);

            float gap = dp(5f);
            float totalWidth = 0f;
            List<Float> pillWidths = new ArrayList<>();
            for (String status : statusPills) {
                float width = shaderTextPaint.measureText(status) + dp(20f);
                pillWidths.add(width);
                totalWidth += width;
            }
            totalWidth += gap * Math.max(0, statusPills.size() - 1);

            float pillTop = top + height + dp(2.5f);
            float cursor = getWidth() / 2f - totalWidth / 2f;
            for (int i = 0; i < statusPills.size(); i++) {
                float width = pillWidths.get(i);
                RectF pill = new RectF(
                        cursor,
                        pillTop,
                        cursor + width,
                        pillTop + dp(15.5f)
                );
                canvas.drawRoundRect(pill, dp(8f), dp(8f), shaderPaint);
                canvas.drawText(
                        statusPills.get(i),
                        cursor + width / 2f,
                        pillTop + dp(10.7f),
                        shaderTextPaint
                );
                cursor += width + gap;
            }
        }
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        setMeasuredDimension(width, (int) dp(52f));
    }
}
