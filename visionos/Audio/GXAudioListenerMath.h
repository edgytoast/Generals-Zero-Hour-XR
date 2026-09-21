// visionOS spatial battlefield audio: pure, allocation-free math shared by the engine (OpenALAudioManager.cpp),
// the host and the host-side unit test (scripts/qa/vision-audio-listener-test.cpp). No engine, OpenAL or
// platform includes: everything here is header-only C++11 so it can be compiled anywhere.
//
// Coordinate conventions (documented once, used everywhere):
//   * GAME WORLD: the engine's world. Units are game units, Z is up, the terrain plane is XY. Sources keep these
//     coordinates (OpenALAudioManager passes them to AL_POSITION unchanged).
//   * BOARD SPACE: metres, origin at the centre of the tabletop board (the point that shows `centerWorld`),
//     +X to the right of the board as seen by the user, +Y toward the far side of the board (the board's
//     "up the map" direction), +Z up out of the board surface. Right handed. This is the space the host
//     converts the head pose into (board pose inverse * room-space head pose).
//   * Board -> world mapping is exactly the inverse of xrWorldToBoard() (GeneralsMD/Code/Main/XrWorld.h):
//         board(x, y, z) = ( rx*wx + ry*wy , -ry*wx + rx*wy , wz ) / span            (board widths, centred)
//     where (rx, ry) is the unit "right" vector of the map view and span is the map span in world units that one
//     board width covers. So the board X axis is world (rx, ry, 0), the board Y axis is world (-ry, rx, 0) (the
//     right vector rotated +90 degrees counter-clockwise about +Z) and both are proper rotations (no mirroring).
//   * metersPerWorldUnit = boardWidthMeters / span. One world unit is that many physical metres on the table.
#ifndef GX_AUDIO_LISTENER_MATH_H
#define GX_AUDIO_LISTENER_MATH_H

#include <cmath>

namespace gxaudio {

struct Vec3 {
	float x, y, z;
};

inline Vec3 make(float x, float y, float z) { Vec3 v = {x, y, z}; return v; }
inline Vec3 add(Vec3 a, Vec3 b) { return make(a.x + b.x, a.y + b.y, a.z + b.z); }
inline Vec3 sub(Vec3 a, Vec3 b) { return make(a.x - b.x, a.y - b.y, a.z - b.z); }
inline Vec3 scale(Vec3 a, float s) { return make(a.x * s, a.y * s, a.z * s); }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline bool finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// Normalises `a`; returns false (and leaves `out` = fallback) for zero, non-finite or denormal-length input.
inline bool normalize(Vec3 a, Vec3 fallback, Vec3 *out) {
	const float n = length(a);
	if (!finite(a) || !(n > 1e-6f)) { *out = fallback; return false; }
	*out = scale(a, 1.0f / n);
	return true;
}

// The board's placement in the game world (what xrWorldToBoard() was built from).
struct BoardFrame {
	Vec3 centerWorld;   // world point shown at the board centre
	float rightX;       // world-plane "right" vector of the map view; need not be normalised
	float rightY;
};

inline BoardFrame defaultBoardFrame() { BoardFrame f = {make(0, 0, 0), 1.0f, 0.0f}; return f; }

// Unit board X/Y axes expressed in world coordinates. Returns false for a degenerate right vector and then
// yields the identity axes so a bad frame can never produce NaNs downstream.
inline bool boardAxes(const BoardFrame &f, Vec3 *xAxis, Vec3 *yAxis) {
	const float len = std::sqrt(f.rightX * f.rightX + f.rightY * f.rightY);
	if (!std::isfinite(len) || !(len > 1e-4f)) {
		*xAxis = make(1, 0, 0);
		*yAxis = make(0, 1, 0);
		return false;
	}
	const float rx = f.rightX / len, ry = f.rightY / len;
	*xAxis = make(rx, ry, 0.0f);
	*yAxis = make(-ry, rx, 0.0f);
	return true;
}

// Board-space direction (any length) -> world direction (same length in board units).
inline Vec3 boardDirToWorld(const BoardFrame &f, Vec3 d) {
	Vec3 ax, ay;
	boardAxes(f, &ax, &ay);
	return add(add(scale(ax, d.x), scale(ay, d.y)), make(0, 0, d.z));
}

// Board-space point in METRES -> world point in game units. metersPerWorldUnit must be > 0.
inline Vec3 boardPointToWorld(const BoardFrame &f, float metersPerWorldUnit, Vec3 p) {
	const float inv = 1.0f / metersPerWorldUnit;
	return add(f.centerWorld, boardDirToWorld(f, scale(p, inv)));
}

// Inverse of boardPointToWorld (used by tests and by hosts that want to draw the listener on the board).
inline Vec3 worldPointToBoard(const BoardFrame &f, float metersPerWorldUnit, Vec3 w) {
	Vec3 ax, ay;
	boardAxes(f, &ax, &ay);
	const Vec3 d = sub(w, f.centerWorld);
	return scale(make(dot(d, ax), dot(d, ay), d.z), metersPerWorldUnit);
}

// The AL listener state in game-world terms.
struct ListenerWorld {
	Vec3 position;          // game units
	Vec3 forward;           // unit "at" vector
	Vec3 up;                // unit "up" vector, orthogonal to forward
	float metersPerUnit;    // AL_METERS_PER_UNIT
};

// Board-space head pose -> game-world listener. Returns false (out untouched) when any input is non-finite
// or the scale is not positive. A forward vector parallel to up (looking straight up/down with a degenerate
// up) is repaired by choosing a stable up rather than being rejected.
inline bool computeListener(const BoardFrame &frame, float metersPerWorldUnit, Vec3 headPosBoard, Vec3 headForwardBoard,
                            Vec3 headUpBoard, ListenerWorld *out) {
	if (!(metersPerWorldUnit > 1e-9f) || !std::isfinite(metersPerWorldUnit) || !finite(headPosBoard) ||
	    !finite(headForwardBoard) || !finite(headUpBoard) || !finite(frame.centerWorld) || !std::isfinite(frame.rightX) ||
	    !std::isfinite(frame.rightY))
		return false;
	Vec3 fwdB;
	if (!normalize(headForwardBoard, make(0, 1, 0), &fwdB)) return false;
	// Orthonormalise up against forward (Gram-Schmidt). Head-up nearly parallel to forward -> pick another axis.
	Vec3 upB = sub(headUpBoard, scale(fwdB, dot(headUpBoard, fwdB)));
	if (!normalize(upB, make(0, 0, 1), &upB)) {
		Vec3 helper = std::fabs(fwdB.z) < 0.9f ? make(0, 0, 1) : make(0, 1, 0);
		upB = sub(helper, scale(fwdB, dot(helper, fwdB)));
		normalize(upB, make(0, 0, 1), &upB);
	}
	out->position = boardPointToWorld(frame, metersPerWorldUnit, headPosBoard);
	Vec3 f, u;
	normalize(boardDirToWorld(frame, fwdB), make(0, 1, 0), &f);
	normalize(boardDirToWorld(frame, upB), make(0, 0, 1), &u);
	out->forward = f;
	out->up = u;
	out->metersPerUnit = metersPerWorldUnit;
	return true;
}

// ---- Distance attenuation ---------------------------------------------------------------------------------

// OpenAL AL_INVERSE_DISTANCE_CLAMPED, the model the engine selects (alDistanceModel in OpenALAudioManager::init).
//   d' = clamp(d, ref, max);  gain = ref / (ref + rolloff * (d' - ref))
inline float inverseDistanceClamped(float distance, float ref, float maxDistance, float rolloff) {
	if (!(ref > 0.0f)) return 1.0f;
	float d = distance < ref ? ref : distance;
	if (maxDistance > ref && d > maxDistance) d = maxDistance;
	return ref / (ref + rolloff * (d - ref));
}

// Tabletop attenuation tuning, in PHYSICAL metres (the head-to-table geometry is physical). Defaults chosen for a
// board about 0.6 to 1.2 m wide viewed from 0.4 to 2 m: everything on the table is within ~1.6 m of the head, so
// with ref 1.0 m / rolloff 0.35 the loudest-to-quietest spread across the table is about 1.5 dB (a battle stays at
// natural level with real panning) while the source still fades gently when the user walks away from the table.
struct TabletopTuning {
	float refDistanceMeters;   // full level at or inside this distance
	float rolloff;             // AL_ROLLOFF_FACTOR
	float maxDistanceMeters;   // level stops falling beyond this distance (clamped model)
	float nominalRangeUnits;   // game-unit event range (AudioSettings global max range) that maps to the values above
	float minRangeScale;       // loud/quiet events scale ref & max by (eventMax / nominalRange), clamped to [min, max]
	float maxRangeScale;
};

inline TabletopTuning defaultTabletopTuning() {
	TabletopTuning t = {1.0f, 0.35f, 8.0f, 600.0f, 0.5f, 2.0f};
	return t;
}

struct SourceAttenuation {
	float refDistance;   // AL_REFERENCE_DISTANCE in game units
	float maxDistance;   // AL_MAX_DISTANCE in game units
	float rolloff;       // AL_ROLLOFF_FACTOR
};

// Per-source values for the AL source, in game units (because sources keep game coordinates and the listener is
// placed in game units). eventMaxRangeUnits is the event's own MaxDistance (0 when unknown -> nominal).
inline SourceAttenuation tabletopAttenuation(const TabletopTuning &t, float metersPerWorldUnit, float eventMaxRangeUnits) {
	float k = 1.0f;
	if (eventMaxRangeUnits > 0.0f && t.nominalRangeUnits > 0.0f) {
		k = eventMaxRangeUnits / t.nominalRangeUnits;
		if (k < t.minRangeScale) k = t.minRangeScale;
		if (k > t.maxRangeScale) k = t.maxRangeScale;
	}
	const float unitsPerMeter = (metersPerWorldUnit > 1e-9f) ? 1.0f / metersPerWorldUnit : 1.0f;
	SourceAttenuation a;
	a.refDistance = t.refDistanceMeters * k * unitsPerMeter;
	a.maxDistance = t.maxDistanceMeters * k * unitsPerMeter;
	if (a.maxDistance < a.refDistance) a.maxDistance = a.refDistance;
	a.rolloff = t.rolloff;
	return a;
}

}  // namespace gxaudio

#endif
