package com.anland.consumer;

import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.Size;

import androidx.annotation.NonNull;
import androidx.camera.core.CameraInfo;
import androidx.camera.core.CameraSelector;
import androidx.camera.core.ImageAnalysis;
import androidx.camera.core.ImageProxy;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.lifecycle.Lifecycle;
import androidx.lifecycle.LifecycleOwner;
import androidx.lifecycle.LifecycleRegistry;

import com.google.common.util.concurrent.ListenableFuture;

import java.nio.ByteBuffer;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * CameraX-based capture bridge. Owns its own {@link Lifecycle} so it can bind
 * use-cases without the host Activity being a LifecycleOwner. Driven entirely from
 * the native control thread (camera_service.c) via the {@code startRecording} /
 * {@code stopRecording} JNI calls — when the Linux producer asks to record, we bind
 * an {@link ImageAnalysis} for that camera and stream YUV420 (packed to I420) frames
 * down the per-camera socket fd, each prefixed by a camera_frame_hdr.
 *
 * Nothing here opens a camera until startRecording is called; init() only discovers
 * the camera count and kicks off the (async) provider initialisation.
 */
public class CameraServices implements LifecycleOwner {
    private static final String TAG = "AnlandCam";

    /* Must match camera_service.h: CAMERA_SLOTS. */
    private static final int SLOTS = 2;

    /* Implemented in camera_service.c (same .so, loaded by MainActivity). nativePackFrame
     * copies the camera's planes straight into the shared-memory slot in their native
     * layout (no chroma de-interleave) and returns the CAM_FMT_*; the slot is then handed
     * to the producer via the READY/DONE handshake instead of copying through a socket. */
    private native int nativeAwaitSlotFree(int cam, int slot, int timeoutMs);
    private native int nativePackFrame(int cam, int slot,
            ByteBuffer y, int yRow, int yPix,
            ByteBuffer u, int uRow, int uPix,
            ByteBuffer v, int vRow, int vPix,
            int w, int h);
    private native void nativeFrameReady(int cam, int slot, int w, int h, int fmt);

    private final LifecycleRegistry lifecycle = new LifecycleRegistry(this);
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private Context appContext;
    private String[] cameraIds = new String[0];
    // Per-camera max YUV_420_888 output resolution, reported to the producer via
    // GET_INFO so it can configure each virtual camera at the real sensor max.
    private int[] maxW = new int[0];
    private int[] maxH = new int[0];

    private ListenableFuture<ProcessCameraProvider> providerFuture;
    private volatile ProcessCameraProvider provider;

    // Per-camera capture state, indexed by camera id (0..cameraIds.length-1).
    private ImageAnalysis[] analyses;
    private ExecutorService[] executors;
    // The shared-memory slot the next frame will be written into (ping-pong).
    private int[] curSlot;

    @NonNull
    @Override
    public Lifecycle getLifecycle() {
        return lifecycle;
    }

    /** Discover cameras and start provider init. Called once, on the main thread. */
    public void init(Context context) {
        appContext = context.getApplicationContext();
        try {
            CameraManager cm = (CameraManager) appContext.getSystemService(Context.CAMERA_SERVICE);
            cameraIds = cm.getCameraIdList();
        } catch (Exception e) {
            Log.e(TAG, "init: getCameraIdList failed", e);
            cameraIds = new String[0];
        }
        int n = cameraIds.length;
        analyses = new ImageAnalysis[n];
        executors = new ExecutorService[n];
        curSlot = new int[n];

        // Precompute each camera's largest YUV_420_888 output size (by area). Done here
        // (synchronously, Camera2) so getCameraMaxWidth/Height are answerable the moment
        // the producer sends GET_INFO.
        maxW = new int[n];
        maxH = new int[n];
        try {
            CameraManager cm = (CameraManager) appContext.getSystemService(Context.CAMERA_SERVICE);
            for (int i = 0; i < n; i++) {
                CameraCharacteristics ch = cm.getCameraCharacteristics(cameraIds[i]);
                StreamConfigurationMap map =
                        ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                if (map == null)
                    continue;
                Size[] sizes = map.getOutputSizes(ImageFormat.YUV_420_888);
                if (sizes == null)
                    continue;
                long bestArea = 0;
                for (Size s : sizes) {
                    long area = (long) s.getWidth() * s.getHeight();
                    if (area > bestArea) {
                        bestArea = area;
                        maxW[i] = s.getWidth();
                        maxH[i] = s.getHeight();
                    }
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "init: querying max resolutions failed", e);
        }
        StringBuilder sb = new StringBuilder("init: cameras=" + n);
        for (int i = 0; i < n; i++)
            sb.append(String.format(" [%d id=%s max=%dx%d]", i, cameraIds[i], maxW[i], maxH[i]));
        Log.i(TAG, sb.toString());

        // Move our lifecycle to RESUMED so bound use-cases become active, and kick
        // off the (async) CameraX provider init. Both must happen on the main thread.
        mainHandler.post(() -> {
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_CREATE);
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_START);
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_RESUME);
            providerFuture = ProcessCameraProvider.getInstance(appContext);
            providerFuture.addListener(() -> {
                try {
                    provider = providerFuture.get();
                    Log.i(TAG, "CameraX provider ready");
                } catch (Exception e) {
                    Log.e(TAG, "provider init failed", e);
                }
            }, command -> mainHandler.post(command));
        });
        Log.i(TAG, "init: " + n + " camera(s)");
    }

    /** Physical camera count. Synchronous; safe to call from the main thread. */
    public int getCameraCount() {
        return cameraIds == null ? 0 : cameraIds.length;
    }

    /** Largest YUV output width for camera {@code index}, or 0 if unknown. */
    public int getCameraMaxWidth(int index) {
        return (index >= 0 && index < maxW.length) ? maxW[index] : 0;
    }

    /** Largest YUV output height for camera {@code index}, or 0 if unknown. */
    public int getCameraMaxHeight(int index) {
        return (index >= 0 && index < maxH.length) ? maxH[index] : 0;
    }

    /**
     * Bind ImageAnalysis for the given camera. Frames are written into the camera's two
     * shared-memory slots (no socket copy). Called on the native control thread, so
     * blocking to await provider readiness here is fine.
     */
    public void startRecording(int cameraId, int width, int height) {
        if (cameraId < 0 || cameraId >= getCameraCount()) {
            Log.e(TAG, "startRecording: bad cameraId " + cameraId);
            return;
        }

        ProcessCameraProvider p = awaitProvider();
        if (p == null) {
            Log.e(TAG, "startRecording: provider unavailable");
            return;
        }

        synchronized (this) {
            stopLocked(cameraId);  // replace any previous binding for this camera
            curSlot[cameraId] = 0;
            executors[cameraId] = Executors.newSingleThreadExecutor();
        }

        ImageAnalysis.Builder b = new ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888);
        if (width > 0 && height > 0)
            b.setTargetResolution(new Size(width, height));
        ImageAnalysis analysis = b.build();
        analysis.setAnalyzer(executors[cameraId], img -> onFrame(cameraId, img));

        CameraSelector selector = selectorForIndex(p, cameraId);

        // bindToLifecycle must run on the main thread.
        runOnMain(() -> {
            try {
                analyses[cameraId] = analysis;
                p.bindToLifecycle(this, selector, analysis);
                Log.i(TAG, "startRecording: bound camera " + cameraId);
            } catch (Exception e) {
                Log.e(TAG, "bindToLifecycle failed for camera " + cameraId, e);
                synchronized (CameraServices.this) { stopLocked(cameraId); }
            }
        });
    }

    /** Stop and unbind the given camera. */
    public void stopRecording(int cameraId) {
        if (cameraId < 0 || cameraId >= getCameraCount())
            return;
        synchronized (this) {
            stopLocked(cameraId);
        }
    }

    /** Stop every active camera (called on producer disconnect / free_resource). */
    public synchronized void stopAllRecording() {
        for (int i = 0; i < getCameraCount(); i++)
            stopLocked(i);
    }

    /** Tear down everything (called from camera_service_destroy on app shutdown). */
    public void release() {
        stopAllRecording();
        runOnMain(() -> {
            if (provider != null)
                provider.unbindAll();
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_DESTROY);
        });
    }

    // --- internals ---

    /** Must hold the monitor. Unbinds the camera and shuts its analyzer executor. */
    private void stopLocked(int cameraId) {
        final ImageAnalysis analysis = analyses[cameraId];
        if (analysis != null) {
            analyses[cameraId] = null;
            runOnMain(() -> {
                if (provider != null)
                    provider.unbind(analysis);
            });
        }
        if (executors[cameraId] != null) {
            executors[cameraId].shutdown();
            executors[cameraId] = null;
        }
    }

    private ProcessCameraProvider awaitProvider() {
        if (provider != null)
            return provider;
        try {
            ListenableFuture<ProcessCameraProvider> f = providerFuture;
            if (f == null)
                f = ProcessCameraProvider.getInstance(appContext);
            provider = f.get();   // safe: not on the main thread
        } catch (Exception e) {
            Log.e(TAG, "awaitProvider failed", e);
        }
        return provider;
    }

    /** Build a CameraSelector pinned to the index-th CameraX camera. */
    private CameraSelector selectorForIndex(ProcessCameraProvider p, int index) {
        final List<CameraInfo> infos = p.getAvailableCameraInfos();
        final CameraInfo target =
                (index < infos.size()) ? infos.get(index) : null;
        if (target == null) {
            // Fall back to the default back camera if the index is out of range.
            return CameraSelector.DEFAULT_BACK_CAMERA;
        }
        return new CameraSelector.Builder()
                .addCameraFilter(list -> {
                    for (CameraInfo ci : list)
                        if (ci == target)
                            return Collections.singletonList(ci);
                    return Collections.emptyList();
                })
                .build();
    }

    /*
     * Analyzer callback. Ping-pong over the two shared-memory slots:
     *   - wait until the producer has released the slot we're about to write (DONE), or
     *     1s elapses (then we overwrite -- drop semantics so a stalled/absent consumer
     *     never wedges CameraX);
     *   - hand the camera planes to native, which copies them into the slot in their
     *     native layout (I420 / NV12 / NV21 -- no chroma de-interleave);
     *   - notify the producer (READY, with the format) and advance to the other slot.
     */
    private void onFrame(int cameraId, ImageProxy image) {
        try {
            final int slot = curSlot[cameraId];
            final int w = image.getWidth();
            final int h = image.getHeight();
            ImageProxy.PlaneProxy[] pl = image.getPlanes();

            nativeAwaitSlotFree(cameraId, slot, 1000);   // wait DONE (or 1s timeout)
            int fmt = nativePackFrame(cameraId, slot,
                    pl[0].getBuffer(), pl[0].getRowStride(), pl[0].getPixelStride(),
                    pl[1].getBuffer(), pl[1].getRowStride(), pl[1].getPixelStride(),
                    pl[2].getBuffer(), pl[2].getRowStride(), pl[2].getPixelStride(),
                    w, h);
            nativeFrameReady(cameraId, slot, w, h, fmt);
            curSlot[cameraId] = slot ^ 1;
        } catch (Exception e) {
            // Drop this frame; recording is torn down via the ctrl channel.
        } finally {
            image.close();
        }
    }

    private void runOnMain(Runnable r) {
        if (Looper.myLooper() == Looper.getMainLooper())
            r.run();
        else
            mainHandler.post(r);
    }
}
