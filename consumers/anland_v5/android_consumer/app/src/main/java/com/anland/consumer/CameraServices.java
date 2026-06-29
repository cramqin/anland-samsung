package com.anland.consumer;

import android.app.Activity;
import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;
import android.util.Size;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.locks.ReentrantLock;

/**
 * Camera2-based capture bridge.  Replaces the previous CameraX implementation with
 * direct Camera2 APIs so the android.camera.extensions / CameraX dependencies are
 * no longer needed.
 *
 * Discoveries cameras synchronously at init() and opens them on-demand when
 * startRecording() is called.  Each opened camera creates an {@link ImageReader}
 * that delivers YUV_420_888 frames; the per-frame callback passes the three planes
 * down to camera_service.c via the same {@code nativePackFrame} / nativeFrameReady
 * handshake used before.
 *
 * Owns no lifecycle registry — Camera2 needs none.  Thread-safety is provided by
 * a ReentrantLock per camera so startRecording / stopRecording can race safely
 * from the native control thread.
 *
 * Protocol (unchanged from the CameraX version):
 *   1. nativeAwaitSlotFree(cam, slot, 1s) — wait until the producer has consumed
 *      the slot (DONE) or 1 s elapses.
 *   2. nativePackFrame(...) — copy Y/U/V ByteBuffers into the shared-memory slot.
 *   3. nativeFrameReady(cam, slot, w, h, fmt) — signal the producer that a frame
 *      is ready (READY).
 *   4. curSlot ^= 1 — advance to the other ping-pong slot.
 */
public class CameraServices {
    private static final String TAG = "AnlandCam";

    /* Must match camera_service.h: CAMERA_SLOTS. */
    private static final int SLOTS = 2;

    static native void nativeInitCameraService(Activity activity);
    static native void nativeDestroyCameraService();

    private native int nativeAwaitSlotFree(int cam, int slot, int timeoutMs);
    private native int nativePackFrame(int cam, int slot,
                                       ByteBuffer y, int yRow, int yPix,
                                       ByteBuffer u, int uRow, int uPix,
                                       ByteBuffer v, int vRow, int vPix,
                                       int w, int h);
    private native void nativeFrameReady(int cam, int slot, int w, int h, int fmt);

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private Context appContext;
    private CameraManager cameraManager;

    private String[] cameraIds = new String[0];
    private int[] maxW = new int[0];
    private int[] maxH = new int[0];

    // --- Per-camera Camera2 state, indexed by logical camera index ---
    private CameraDevice[] devices;
    private CameraCaptureSession[] sessions;
    private ImageReader[] readers;
    private ExecutorService[] executors;
    private HandlerThread[] bgThreads;
    private Handler[] bgHandlers;
    private int[] curSlot;                 // next shared-memory slot to fill
    private ReentrantLock[] locks;   // guard start/stop races

    public CameraServices() {
        locks = new ReentrantLock[0];
    }

    /** Discover cameras and compute max YUV_420_888 resolutions. Called once, main thread. */
    public void init(Context context) {
        appContext = context.getApplicationContext();
        cameraManager = (CameraManager) appContext.getSystemService(Context.CAMERA_SERVICE);
        if (cameraManager == null) {
            Log.e(TAG, "init: no CameraManager");
            cameraIds = new String[0];
            return;
        }
        try {
            cameraIds = cameraManager.getCameraIdList();
        } catch (Exception e) {
            Log.e(TAG, "init: getCameraIdList failed", e);
            cameraIds = new String[0];
        }
        int n = cameraIds.length;
        devices = new CameraDevice[n];
        sessions = new CameraCaptureSession[n];
        readers = new ImageReader[n];
        executors = new ExecutorService[n];
        bgThreads = new HandlerThread[n];
        bgHandlers = new Handler[n];
        curSlot = new int[n];
        // Grow the lock array if needed; init() is called once so n is final.
        if (n != locks.length) {
            ReentrantLock[] grown = new ReentrantLock[n];
            for (int i = 0; i < n; i++) {
                grown[i] = (i < locks.length) ? locks[i] : new ReentrantLock();
            }
            locks = grown;
        }

        maxW = new int[n];
        maxH = new int[n];
        try {
            for (int i = 0; i < n; i++) {
                CameraCharacteristics ch = cameraManager.getCameraCharacteristics(cameraIds[i]);
                StreamConfigurationMap map =
                        ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                if (map == null) continue;
                Size[] sizes = map.getOutputSizes(ImageFormat.YUV_420_888);
                if (sizes == null) continue;
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
        for (int i = 0; i < n; i++) {
            sb.append(String.format(" [%d id=%s max=%dx%d]", i, cameraIds[i], maxW[i], maxH[i]));
        }
        Log.i(TAG, sb.toString());

        Log.i(TAG, "init: " + n + " camera(s)");
    }

    public int getCameraCount() {
        return cameraIds == null ? 0 : cameraIds.length;
    }

    public int getCameraMaxWidth(int index) {
        return (index >= 0 && index < maxW.length) ? maxW[index] : 0;
    }

    public int getCameraMaxHeight(int index) {
        return (index >= 0 && index < maxH.length) ? maxH[index] : 0;
    }

    // ---------------------------------------------------------------
    // startRecording / stopRecording — called from the native control
    // thread (not the main thread).
    // ---------------------------------------------------------------

    /**
     * Open the camera and start streaming YUV frames into the shared-memory slots.
     * Blocks synchronously until the CameraDevice is opened and a capture session
     * is active (or fails).
     */
    public void startRecording(int cameraId, int width, int height) {
        if (cameraId < 0 || cameraId >= getCameraCount()) {
            Log.e(TAG, "startRecording: bad cameraId " + cameraId);
            return;
        }
        final int id = cameraId;
        final int w = (width > 0) ? width : maxW[id];
        final int h = (height > 0) ? height : maxH[id];

        locks[id].lock();
        try {
            // If already running, stop first.
            stopLocked(id);

            // 1. Create a background thread for this camera.
            HandlerThread ht = new HandlerThread("camera-" + id);
            ht.start();
            bgThreads[id] = ht;
            Handler bgHandler = new Handler(ht.getLooper());
            bgHandlers[id] = bgHandler;

            // 2. Create the ImageReader at the requested resolution.
            ImageReader reader = ImageReader.newInstance(
                    Math.max(w, 1), Math.max(h, 1),
                    ImageFormat.YUV_420_888,
                    2 /* maxImages — use 2 for ping-pong */);
            reader.setOnImageAvailableListener(this::onFrameAvailable, bgHandler);
            readers[id] = reader;

            // 3. Open the camera device (synchronously, blocking this thread).
            final CameraDevice[] outDevice = new CameraDevice[1];
            final boolean[] opened = {false};
            final boolean[] failed = {false};

            try {
                cameraManager.openCamera(cameraIds[id],
                        new CameraDevice.StateCallback() {
                            @Override
                            public void onOpened(CameraDevice cd) {
                                synchronized (opened) {
                                    outDevice[0] = cd;
                                    opened[0] = true;
                                    opened.notifyAll();
                                }
                            }
                            @Override
                            public void onDisconnected(CameraDevice cd) {
                                Log.w(TAG, "camera " + id + " disconnected");
                                cd.close();
                                synchronized (opened) {
                                    failed[0] = true;
                                    opened[0] = true;
                                    opened.notifyAll();
                                }
                            }
                            @Override
                            public void onError(CameraDevice cd, int error) {
                                Log.e(TAG, "camera " + id + " onError=" + error);
                                cd.close();
                                synchronized (opened) {
                                    failed[0] = true;
                                    opened[0] = true;
                                    opened.notifyAll();
                                }
                            }
                        }, bgHandler);
            } catch (Exception e) {
                Log.e(TAG, "openCamera failed for camera " + id, e);
                return;
            }

            // Wait for the camera to open (up to 3 s).
            synchronized (opened) {
                long deadline = System.currentTimeMillis() + 3000;
                while (!opened[0]) {
                    long remaining = deadline - System.currentTimeMillis();
                    if (remaining <= 0) break;
                    try {
                        opened.wait(remaining);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }
            if (failed[0] || outDevice[0] == null) {
                Log.e(TAG, "startRecording: camera " + id + " open failed");
                stopLocked(id);
                return;
            }
            devices[id] = outDevice[0];

            // 4. Create a capture session with only the ImageReader surface.
            // YUV_420_888 via ImageReader is a valid output target — no preview surface needed.
            final Surface readerSurface = readers[id].getSurface();

            final CameraCaptureSession[] outSession = new CameraCaptureSession[1];
            final boolean[] sessionReady = {false};

            try {
                devices[id].createCaptureSession(
                        java.util.Collections.singletonList(readerSurface),
                        new CameraCaptureSession.StateCallback() {
                            @Override
                            public void onConfigured(CameraCaptureSession cs) {
                                synchronized (sessionReady) {
                                    outSession[0] = cs;
                                    sessionReady[0] = true;
                                    sessionReady.notifyAll();
                                }
                            }
                            @Override
                            public void onConfigureFailed(CameraCaptureSession cs) {
                                Log.e(TAG, "createCaptureSession failed for camera " + id);
                                cs.close();
                                synchronized (sessionReady) {
                                    sessionReady[0] = true;
                                    sessionReady.notifyAll();
                                }
                            }
                        }, bgHandler);
            } catch (Exception e) {
                Log.e(TAG, "createCaptureSession threw for camera " + id, e);
                stopLocked(id);
                return;
            }

            // Wait for session (up to 3 s).
            synchronized (sessionReady) {
                long deadline = System.currentTimeMillis() + 3000;
                while (!sessionReady[0]) {
                    long remaining = deadline - System.currentTimeMillis();
                    if (remaining <= 0) break;
                    try {
                        sessionReady.wait(remaining);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }
            if (outSession[0] == null) {
                Log.e(TAG, "startRecording: session not ready for camera " + id);
                stopLocked(id);
                return;
            }
            sessions[id] = outSession[0];

            // 5. Build a repeating TEMPLATE_RECORD request targeting our reader surface.
            try {
                CaptureRequest.Builder reqBuilder =
                        devices[id].createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                reqBuilder.addTarget(readerSurface);
                // Disable 3A convergence delays for minimal latency.
                reqBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                        CaptureRequest.CONTROL_AE_MODE_ON);
                reqBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                        CaptureRequest.CONTROL_AWB_MODE_AUTO);
                reqBuilder.set(CaptureRequest.CONTROL_AF_MODE,
                        CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
                sessions[id].setRepeatingRequest(reqBuilder.build(), null, bgHandler);
            } catch (Exception e) {
                Log.e(TAG, "setRepeatingRequest failed for camera " + id, e);
                stopLocked(id);
                return;
            }

            curSlot[id] = 0;
            Log.i(TAG, "startRecording: camera " + id + " started " + w + "x" + h);
        } finally {
            locks[id].unlock();
        }
    }

    /** Stop the given camera and release all Camera2 resources. */
    public void stopRecording(int cameraId) {
        if (cameraId < 0 || cameraId >= getCameraCount()) return;
        locks[cameraId].lock();
        try {
            stopLocked(cameraId);
        } finally {
            locks[cameraId].unlock();
        }
    }

    /** Stop every active camera. */
    public synchronized void stopAllRecording() {
        for (int i = 0; i < getCameraCount(); i++) {
            locks[i].lock();
            try {
                stopLocked(i);
            } finally {
                locks[i].unlock();
            }
        }
    }

    /** Tear down everything (called on app shutdown). */
    public void release() {
        stopAllRecording();
    }

    // ---------------------------------------------------------------
    // Internals
    // ---------------------------------------------------------------

    /** Must hold {@link #locks}[cameraId]. */
    private void stopLocked(int id) {
        // Stop repeating request first.
        CameraCaptureSession s = sessions[id];
        if (s != null) {
            try {
                s.stopRepeating();
            } catch (Exception ignored) {
            }
            sessions[id] = null;
            s.close();
        }
        // Close the reader.
        ImageReader r = readers[id];
        if (r != null) {
            readers[id] = null;
            r.close();
        }
        // Close the camera device.
        CameraDevice d = devices[id];
        if (d != null) {
            devices[id] = null;
            d.close();
        }
        // Quit the background thread.
        HandlerThread ht = bgThreads[id];
        if (ht != null) {
            bgThreads[id] = null;
            bgHandlers[id] = null;
            try {
                ht.quitSafely();
            } catch (Exception ignored) {
            }
        }
        ExecutorService ex = executors[id];
        if (ex != null) {
            executors[id] = null;
            ex.shutdown();
        }
    }

    /**
     * ImageReader callback.  Runs on per-camera background HandlerThread.
     * Acquires the latest image, copies its planes into the shared-memory
     * slot via the native handshake, then closes the image.
     */
    private void onFrameAvailable(ImageReader reader) {
        // Find which camera this reader belongs to.
        int cameraId = -1;
        for (int i = 0; i < readers.length; i++) {
            if (readers[i] == reader) {
                cameraId = i;
                break;
            }
        }
        if (cameraId < 0) return;

        Image image = null;
        try {
            image = reader.acquireLatestImage();
        } catch (Exception e) {
            // ImageReader may have been closed concurrently.
            return;
        }
        if (image == null) return;

        try {
            final int slot = curSlot[cameraId];
            final int w = image.getWidth();
            final int h = image.getHeight();
            Image.Plane[] planes = image.getPlanes();

            if (planes.length < 3) {
                Log.w(TAG, "onFrameAvailable: expected 3 planes, got " + planes.length);
                return;
            }

            nativeAwaitSlotFree(cameraId, slot, 1000);
            int fmt = nativePackFrame(cameraId, slot,
                    planes[0].getBuffer(), planes[0].getRowStride(), planes[0].getPixelStride(),
                    planes[1].getBuffer(), planes[1].getRowStride(), planes[1].getPixelStride(),
                    planes[2].getBuffer(), planes[2].getRowStride(), planes[2].getPixelStride(),
                    w, h);
            nativeFrameReady(cameraId, slot, w, h, fmt);
            curSlot[cameraId] = slot ^ 1;
        } catch (Exception e) {
            Log.e(TAG, "onFrameAvailable error for camera " + cameraId, e);
        } finally {
            image.close();
        }
    }
}
