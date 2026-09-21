// visionOS interaction layer: the forwarding VisionEngineBridge (thin, no logic).
//
// VisionXrGameBootBridge implements every VisionEngineBridge method by calling the XrGameBoot_* function of the
// same name (the 1:1 table is docs/visionos-interaction.md, section 7). It is compiled into the engine library
// (z_generals picks VisionEngineBridgeXr.cpp up through the visionos/*.cpp glob) and only has a body when
// GX_XR_HOST is defined (Android, or visionOS with GX_PLATFORM_VISIONOS); everywhere else the factory returns nullptr.
//
// It needs nothing from the shell: the Objective-C++ engine host (package C) creates one instance on the render
// thread and hands it to VisionInteraction:
//
//     static VisionInteraction interaction(VisionCreateXrGameBootBridge());
//
// Threading is the engine host's: every method must run on the render thread that owns the engine.
#pragma once

#include "VisionEngineBridge.h"

// Process-lifetime singleton (the XrGameBoot_* API is itself global state). Returns nullptr when the build has no
// XR engine host (GX_XR_HOST undefined).
VisionEngineBridge *VisionCreateXrGameBootBridge();
