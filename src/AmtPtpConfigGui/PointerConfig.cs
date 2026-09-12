using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    [StructLayout(LayoutKind.Sequential)]
    public struct PointerConfig
    {
        public uint StructVersion;
        public uint ForceTapThreshold;
        public uint ForceTapAction;
        public uint ForceTouchEnabled;
        public uint RequirePressureToActivate;
        public uint RequirePressureContinuously;
        public uint CursorSmoothingPercent;
        public uint CursorSpeedPercent;
        public uint CursorDeadzone;
        public uint CursorDeadzoneSlow;
        public uint CursorDeadzoneFast;
        public uint CursorSlowVelocity;
        public uint CursorFastVelocity;
        public uint SmoothingAlphaDen;
        public uint SmoothingAlphaNumSlow;
        // --- Small-contact size filtering ---------------------------------
        // Two INDEPENDENT gates, each with its own on/off flag and its own
        // GUI-tunable "M" (Major axis) threshold. Neither depends on the
        // other being enabled, and both are available for non-Force-Touch
        // AND Force-Touch devices. See the matching AMT_POINTER_CONFIG
        // fields/comments in Public.h.
        public uint SmallContactRejectionEnabled;           // birth-only: on/off
        public uint SmallContactBirthMajorThreshold;         // birth-only: M
        public uint SmallContactRejectionStrict;             // continuous: on/off
        public uint SmallContactMajorThreshold;              // continuous: M

        public uint ForceTouchSmallContactRejectionEnabled;      // birth-only: on/off
        public uint ForceTouchSmallContactBirthMajorThreshold;   // birth-only: M
        public uint ForceTouchSmallContactRejectionStrict;       // continuous: on/off
        public uint ForceTouchSmallContactMajorThreshold;        // continuous: M

        // Software Force Touch emulation (non-Force-Touch trackpads only -
        // see the matching AMT_POINTER_CONFIG fields in Public.h).
        public uint ForceTouchEmulationEnabled;
        public uint ForceTouchEmulationAction;
        public uint ForceTouchEmulationHoldMs;

        // Drag-cancel distance (raw sensor units) - independent per path,
        // see the matching AMT_POINTER_CONFIG fields in Public.h for why.
        public uint ForceTapDragLockoutDistance;
        public uint ForceTouchEmulationDragLockoutDistance;

        public const uint CurrentVersion = 11;
        public const uint ActionContextMenu = 0;
        public const uint ActionMiddleClick = 1;
        public const uint ActionDoubleClick = 2;
        public const uint ThresholdMin = 200;
        public const uint ThresholdMax = 400;
        public const uint ActionMax = 2;
        public const uint EmulationHoldMsMin = 200;
        public const uint EmulationHoldMsMax = 2000;
        public const uint EmulationHoldMsStep = 50;
        public const uint DragLockoutDistanceMin = 40;
        public const uint DragLockoutDistanceMax = 400;
        public const uint DragLockoutDistanceStep = 10;

        // "M" (Major axis) small-contact cutoff range - shared by the
        // non-Force-Touch and Force-Touch sliders.
        public const uint SmallContactMajorMin = 10;
        public const uint SmallContactMajorMax = 150;

        public static PointerConfig Default => new PointerConfig
        {
            StructVersion = CurrentVersion,
            ForceTapThreshold = 265,
            ForceTapAction = ActionContextMenu,
            ForceTouchEnabled = 1,
            RequirePressureToActivate = 1,
            RequirePressureContinuously = 0,
            CursorSmoothingPercent = 0,
            CursorSpeedPercent = 100,
            CursorDeadzone = 1,
            CursorDeadzoneSlow = 4,
            CursorDeadzoneFast = 0,
            CursorSlowVelocity = 110,
            CursorFastVelocity = 905,
            SmoothingAlphaDen = 8,
            SmoothingAlphaNumSlow = 3,
            SmallContactRejectionEnabled = 1,
            SmallContactBirthMajorThreshold = 80,
            SmallContactRejectionStrict = 0,
            SmallContactMajorThreshold = 50,
            ForceTouchSmallContactRejectionEnabled = 0,
            ForceTouchSmallContactBirthMajorThreshold = 80,
            ForceTouchSmallContactRejectionStrict = 0,
            ForceTouchSmallContactMajorThreshold = 50,
            ForceTouchEmulationEnabled = 1,
            ForceTouchEmulationAction = ActionContextMenu,
            ForceTouchEmulationHoldMs = 300,
            ForceTapDragLockoutDistance = 110,
            ForceTouchEmulationDragLockoutDistance = 110,
        };

        public PointerConfig Clamped()
        {
            PointerConfig c = this;
            c.StructVersion = CurrentVersion;
            c.ForceTapThreshold = Clamp(c.ForceTapThreshold, ThresholdMin, ThresholdMax);
            if (c.ForceTapAction > ActionMax) c.ForceTapAction = ActionContextMenu;
            c.ForceTouchEnabled = c.ForceTouchEnabled != 0 ? 1u : 0u;
            c.RequirePressureToActivate = c.RequirePressureToActivate != 0 ? 1u : 0u;
            c.RequirePressureContinuously = c.RequirePressureContinuously != 0 ? 1u : 0u;
            // Two INDEPENDENT small-contact gates per device kind - neither
            // flag forces the other off. Each has its own "M" threshold.
            c.SmallContactRejectionEnabled = c.SmallContactRejectionEnabled != 0 ? 1u : 0u;
            c.SmallContactBirthMajorThreshold = Clamp(c.SmallContactBirthMajorThreshold, SmallContactMajorMin, SmallContactMajorMax);
            c.SmallContactRejectionStrict = c.SmallContactRejectionStrict != 0 ? 1u : 0u;
            c.SmallContactMajorThreshold = Clamp(c.SmallContactMajorThreshold, SmallContactMajorMin, SmallContactMajorMax);

            c.ForceTouchSmallContactRejectionEnabled = c.ForceTouchSmallContactRejectionEnabled != 0 ? 1u : 0u;
            c.ForceTouchSmallContactBirthMajorThreshold = Clamp(c.ForceTouchSmallContactBirthMajorThreshold, SmallContactMajorMin, SmallContactMajorMax);
            c.ForceTouchSmallContactRejectionStrict = c.ForceTouchSmallContactRejectionStrict != 0 ? 1u : 0u;
            c.ForceTouchSmallContactMajorThreshold = Clamp(c.ForceTouchSmallContactMajorThreshold, SmallContactMajorMin, SmallContactMajorMax);
            c.CursorSmoothingPercent = Clamp(c.CursorSmoothingPercent, 0, 100);
            c.CursorSpeedPercent = Clamp(c.CursorSpeedPercent, 50, 200);
            c.CursorDeadzone = Clamp(c.CursorDeadzone, 0, 8);
            c.CursorDeadzoneSlow = Clamp(c.CursorDeadzoneSlow, 0, 8);
            c.CursorDeadzoneFast = Clamp(c.CursorDeadzoneFast, 0, 8);
            c.CursorSlowVelocity = Clamp(c.CursorSlowVelocity, 20, 300);
            c.CursorFastVelocity = Clamp(c.CursorFastVelocity, 200, 2000);
            if (c.CursorFastVelocity <= c.CursorSlowVelocity) c.CursorFastVelocity = c.CursorSlowVelocity + 1;
            c.SmoothingAlphaDen = Clamp(c.SmoothingAlphaDen, 1, 16);
            c.SmoothingAlphaNumSlow = Clamp(c.SmoothingAlphaNumSlow, 1, 16);
            if (c.SmoothingAlphaNumSlow > c.SmoothingAlphaDen) c.SmoothingAlphaNumSlow = c.SmoothingAlphaDen;

            c.ForceTouchEmulationEnabled = c.ForceTouchEmulationEnabled != 0 ? 1u : 0u;
            if (c.ForceTouchEmulationAction > ActionMax) c.ForceTouchEmulationAction = ActionContextMenu;
            c.ForceTouchEmulationHoldMs = ClampToGrid(c.ForceTouchEmulationHoldMs, EmulationHoldMsMin, EmulationHoldMsMax, EmulationHoldMsStep);

            c.ForceTapDragLockoutDistance = ClampToGrid(c.ForceTapDragLockoutDistance, DragLockoutDistanceMin, DragLockoutDistanceMax, DragLockoutDistanceStep);

            c.ForceTouchEmulationDragLockoutDistance = ClampToGrid(c.ForceTouchEmulationDragLockoutDistance, DragLockoutDistanceMin, DragLockoutDistanceMax, DragLockoutDistanceStep);

            return c;
        }

        private static uint Clamp(uint v, uint min, uint max) => v < min ? min : (v > max ? max : v);

        // Clamp, then re-quantize onto the Step grid the corresponding GUI
        // slider steps in (round to nearest, then clamp again in case
        // rounding pushed an edge value one step out of range). Mirrors
        // AmtClampToStepGrid in the driver's ConfigIoctl.c - replaces three
        // near-identical clamp/round/clamp blocks that used to live inline
        // here.
        private static uint ClampToGrid(uint v, uint min, uint max, uint step)
        {
            v = Clamp(v, min, max);
            v = ((v + step / 2) / step) * step;
            return Clamp(v, min, max);
        }
    }
}