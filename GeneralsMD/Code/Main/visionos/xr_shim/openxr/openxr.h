// visionOS / host-test stand-in for <openxr/openxr.h>.
//
// The "pure" Quest XR headers (XrMath.h, XrPlacement.h, XrWorld.h, XrTactics.h,
// XrLayers.h, XrPanelLayout.h, ...) depend on OpenXR only for a handful of
// plain-old-data types and eight flag bits. This file provides exactly those,
// with the same field names and layout, so the decision logic can be compiled
// unchanged on Apple platforms (visionOS engine library, macOS host tests).
//
// Put the directory CONTAINING "openxr/" (that is, this xr_shim directory) on the
// include path ONLY for non-OpenXR builds. Never combine it with the real OpenXR
// SDK headers on the same include path: the real <openxr/openxr.h> defines the
// same names, and mixing them would be an ODR violation. The Quest (Android)
// build keeps using the real SDK and never sees this file.
//
// Only what the pure headers touch is present. Anything that needs handles,
// sessions, swapchains or extensions (XrControls.h, XrScene.h, XrSceneUI.h,
// XrHello.cpp) is deliberately NOT supported.
#pragma once
#include <cstdint>

typedef int64_t XrTime;
typedef uint64_t XrViewStateFlags;
typedef uint64_t XrSpaceLocationFlags;

typedef struct XrVector2f { float x, y; } XrVector2f;
typedef struct XrVector3f { float x, y, z; } XrVector3f;
typedef struct XrQuaternionf { float x, y, z, w; } XrQuaternionf;
typedef struct XrPosef { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; } XrFovf;
typedef struct XrView { int type; const void *next; XrPosef pose; XrFovf fov; } XrView;
typedef struct XrRect2Df { XrVector2f offset; struct { float width, height; } extent; } XrRect2Df;
typedef struct XrRect3DfFB { XrVector3f offset; struct { float width, height, depth; } extent; } XrRect3DfFB;
typedef struct XrEventDataReferenceSpaceChangePending {
	int type; const void *next; void *session; int referenceSpaceType;
	XrTime changeTime; uint32_t poseValid; XrPosef poseInPreviousSpace;
} XrEventDataReferenceSpaceChangePending;

#define XR_VIEW_STATE_ORIENTATION_VALID_BIT 1ull
#define XR_VIEW_STATE_POSITION_VALID_BIT 2ull
#define XR_VIEW_STATE_ORIENTATION_TRACKED_BIT 4ull
#define XR_VIEW_STATE_POSITION_TRACKED_BIT 8ull
#define XR_SPACE_LOCATION_ORIENTATION_VALID_BIT 1ull
#define XR_SPACE_LOCATION_POSITION_VALID_BIT 2ull
#define XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT 4ull
#define XR_SPACE_LOCATION_POSITION_TRACKED_BIT 8ull
