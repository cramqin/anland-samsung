package com.anland.consumer;

import android.content.Context;
import android.view.MotionEvent;
import android.view.ViewConfiguration;

/**
 * Laptop-style virtual touchpad: interprets finger gestures on the surface as
 * relative mouse motion, taps/clicks, long-press drag and two-finger scroll,
 * forwarding them through a host-provided {@link Output}.
 *
 * Self-contained state machine — the host routes non-mouse touches here (see
 * MainActivity.onTouchEvent) when touchpad mode is on, pushes the acceleration
 * preference via {@link #setAccelStrength}. Each physical input stream should use
 * its own instance so gesture state cannot leak between devices; their outputs may
 * still share one cursor controller.
 */
public final class VirtualTouchpad {

    /**
     * Gesture output supplied by the host. Keeping coordinates and button state
     * outside this recognizer lets the on-screen and captured touchpads share one
     * remote cursor without sharing their state machines.
     */
    interface Output {
        void onMotion(float dx, float dy);
        void onScroll(int axis, float value);
        void onButton(int button, boolean pressed);
    }

    // 状态机
    private static final int STATE_IDLE = 0;
    private static final int STATE_ONE_FINGER = 1;
    private static final int STATE_TWO_FINGER = 2;
    private static final int STATE_DRAGGING = 3;
    private int currentState = STATE_IDLE;

    private float lastX1, lastY1;
    private float startX1, startY1;
    private float lastX2, lastY2;
    private long downTime1;
    private long twoFingerDownTime;
    private final float touchSlop;

    private boolean isSingleTapCandidate = false;
    private boolean isTwoFingerTapCandidate = false;
    private boolean isDraggingActive = false;

    private static final long TOUCH_LONG_PRESS_TIMEOUT = 500;
    private boolean hasLongPressed = false;
    private boolean isLongPressPossible = false;
    private boolean isMultiFinger = false;

    private float mouseAccelStrength = 1.0f; // 加速度强度，0.5 ~ 10.0

    // ===== 调整后的平滑/抗抖动参数（更灵敏、更连续） =====
    private static final float DEAD_ZONE = 0.3f;          // 死区从 0.5 降到 0.3
    private static final float SMOOTHING_FACTOR = 0.45f;   // 提高响应速度
    private static final float ACCUMULATED_THRESHOLD = 0.1f; // 从 0.8 大幅降低，让移动更连续

    private float smoothedDx = 0f;
    private float smoothedDy = 0f;
    private float accumulatedX = 0f;
    private float accumulatedY = 0f;
    private boolean smoothInitialized = false;

    private final Output output;

    VirtualTouchpad(Context context, Output output) {
        if (output == null)
            throw new IllegalArgumentException("VirtualTouchpad output must not be null");
        this.output = output;
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
    }

    /** Set the acceleration strength (clamped to 0.5 ~ 10.0). */
    void setAccelStrength(float strength) {
        mouseAccelStrength = Math.max(0.5f, Math.min(10.0f, strength));
    }

    /** Drop any smoothing history after a surface resize. */
    void onSurfaceChanged() {
        resetSmoothing();
    }

    /** Cancel an in-progress gesture when the capture window loses focus. */
    void cancel() {
        if (isDraggingActive)
            sendButton(0x110, false);
        resetTouchpadState();
        resetSmoothing();
    }

    private void sendButton(int button, boolean pressed) {
        output.onButton(button, pressed);
    }

    private void sendScroll(int axis, float value) {
        output.onScroll(axis, value);
    }

    /**
     * Send relative movement to the shared cursor backend.
     */
    private void sendMotion(float dx, float dy) {
        output.onMotion(dx, dy);
    }

    // ==================== 触摸板手势及辅助方法 ====================
    boolean onTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();

        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                // Recover from a missing CANCEL/UP before starting a new stream.
                if (isDraggingActive)
                    sendButton(0x110, false);
                float x = event.getX();
                float y = event.getY();
                startX1 = lastX1 = x;
                startY1 = lastY1 = y;
                downTime1 = event.getEventTime();
                twoFingerDownTime = 0L;
                hasLongPressed = false;
                isLongPressPossible = true;
                isSingleTapCandidate = true;
                isTwoFingerTapCandidate = false;
                isDraggingActive = false;
                isMultiFinger = false;
                currentState = STATE_ONE_FINGER;
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                isMultiFinger = true;
                isSingleTapCandidate = false;
                isLongPressPossible = false;
                if (currentState == STATE_DRAGGING) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                }
                if (pointerCount == 2) {
                    currentState = STATE_TWO_FINGER;
                    isTwoFingerTapCandidate = true;
                    twoFingerDownTime = event.getEventTime();
                    lastX1 = event.getX(0);
                    lastY1 = event.getY(0);
                    lastX2 = event.getX(1);
                    lastY2 = event.getY(1);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                if (pointerCount == 1 && !isMultiFinger) {
                    float x = event.getX();
                    float y = event.getY();
                    float rawDx = x - lastX1;
                    float rawDy = y - lastY1;
                    float dist = (float) Math.hypot(x - startX1, y - startY1);

                    if (dist > touchSlop) {
                        isLongPressPossible = false;
                        isSingleTapCandidate = false;
                        // Clear this once the remaining finger actually moves so
                        // a scroll/drag cannot finish as a right-click.
                        isTwoFingerTapCandidate = false;
                    }

                    if (isLongPressPossible && !hasLongPressed &&
                            (event.getEventTime() - downTime1) >= TOUCH_LONG_PRESS_TIMEOUT) {
                        hasLongPressed = true;
                        currentState = STATE_DRAGGING;
                        isDraggingActive = true;
                        sendButton(0x110, true);
                        output.onMotion(0f, 0f);
                        resetSmoothing();
                        break;
                    }

                    float[] smoothed = applySmoothing(rawDx, rawDy);
                    float smoothDx = smoothed[0];
                    float smoothDy = smoothed[1];

                    if (smoothDx != 0f || smoothDy != 0f) {
                        // 计算移动距离（平滑后的欧式距离）
                        float distance = (float) Math.hypot(smoothDx, smoothDy);

                        // 改进的加速度曲线：以 10px 为参考阈值，使小位移也能获得明显加速
                        float speedFactor = distance / 10.0f;
                        // 使用 sigmoid-like 曲线：scale = 1 + (strength - 1) * (speed / (1 + speed))
                        float dynamicScale = 1.0f + (mouseAccelStrength - 1.0f) * (speedFactor / (1.0f + speedFactor));
                        // 限制范围，防止失控（最大不超过 10 倍）
                        dynamicScale = Math.max(0.3f, Math.min(10.0f, dynamicScale));

                        float moveX = smoothDx * dynamicScale;
                        float moveY = smoothDy * dynamicScale;
                        sendMotion(moveX, moveY);
                    }

                    lastX1 = x;
                    lastY1 = y;

                } else if (pointerCount == 2) {
                    if (currentState == STATE_TWO_FINGER) {
                        float x1 = event.getX(0);
                        float y1 = event.getY(0);
                        float x2 = event.getX(1);
                        float y2 = event.getY(1);
                        float avgDx = ((x1 - lastX1) + (x2 - lastX2)) / 2;
                        float avgDy = ((y1 - lastY1) + (y2 - lastY2)) / 2;

                        if (Math.abs(avgDx) > 1 || Math.abs(avgDy) > 1) {
                            isTwoFingerTapCandidate = false;
                            if (Math.abs(avgDy) > Math.abs(avgDx) * 0.5) {
                                sendScroll(0, -avgDy * 0.5f);
                            }
                            if (Math.abs(avgDx) > Math.abs(avgDy) * 0.5) {
                                sendScroll(1, avgDx * 0.5f);
                            }
                            lastX1 = x1;
                            lastY1 = y1;
                            lastX2 = x2;
                            lastY2 = y2;
                        }
                    }
                }
                break;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                int remaining = pointerCount - 1;
                if (remaining == 1) {
                    isMultiFinger = false;
                    isSingleTapCandidate = false;
                    isLongPressPossible = false;
                    int idx = (event.getActionIndex() == 0) ? 1 : 0;
                    lastX1 = event.getX(idx);
                    lastY1 = event.getY(idx);
                    startX1 = lastX1;
                    startY1 = lastY1;
                    downTime1 = event.getEventTime();
                    hasLongPressed = false;
                    currentState = STATE_ONE_FINGER;
                    resetSmoothing();
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                long duration = event.getEventTime() - downTime1;
                boolean isQuickTap = duration < 300;
                boolean isQuickTwoFingerTap = twoFingerDownTime > 0L
                        && event.getEventTime() - twoFingerDownTime < 300;

                if (isDraggingActive) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }

                if (isTwoFingerTapCandidate && isQuickTwoFingerTap) {
                    sendButton(0x111, true);
                    sendButton(0x111, false);
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }

                if (currentState == STATE_ONE_FINGER && isSingleTapCandidate && isQuickTap) {
                    // Each physical tap is one click. Two quick taps naturally form
                    // a double-click at the remote compositor; synthesizing another
                    // pair here would turn two taps into three clicks.
                    sendButton(0x110, true);
                    sendButton(0x110, false);
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }
                resetTouchpadState();
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                if (isDraggingActive) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                }
                resetTouchpadState();
                resetSmoothing();
                break;
            }
        }
        return true;
    }

    private void resetTouchpadState() {
        currentState = STATE_IDLE;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        hasLongPressed = false;
        isDraggingActive = false;
        isLongPressPossible = false;
        isMultiFinger = false;
        twoFingerDownTime = 0L;
    }

    private void resetSmoothing() {
        smoothedDx = 0f;
        smoothedDy = 0f;
        accumulatedX = 0f;
        accumulatedY = 0f;
        smoothInitialized = false;
    }

    private float[] applySmoothing(float rawDx, float rawDy) {
        float deadDx = Math.abs(rawDx) < DEAD_ZONE ? 0f : rawDx;
        float deadDy = Math.abs(rawDy) < DEAD_ZONE ? 0f : rawDy;

        if (deadDx == 0f && deadDy == 0f) {
            return new float[]{0f, 0f};
        }

        if (!smoothInitialized) {
            smoothedDx = deadDx;
            smoothedDy = deadDy;
            smoothInitialized = true;
        } else {
            smoothedDx = SMOOTHING_FACTOR * deadDx + (1 - SMOOTHING_FACTOR) * smoothedDx;
            smoothedDy = SMOOTHING_FACTOR * deadDy + (1 - SMOOTHING_FACTOR) * smoothedDy;
        }

        // 累积阈值大幅降低，让移动更加连续
        accumulatedX += smoothedDx;
        accumulatedY += smoothedDy;

        float outX = 0f;
        float outY = 0f;
        if (Math.abs(accumulatedX) >= ACCUMULATED_THRESHOLD) {
            outX = accumulatedX;
            accumulatedX = 0f;
        }
        if (Math.abs(accumulatedY) >= ACCUMULATED_THRESHOLD) {
            outY = accumulatedY;
            accumulatedY = 0f;
        }
        return new float[]{outX, outY};
    }

}
