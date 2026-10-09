#pragma once
// Per-node GL viewports and cameras (canvas, panel, projector, retired).
// Split out of AppShared.h so NodeViewport.h and, through it,
// Geometry3DNodes.h are not in every app/ TU
// (docs/plans/compile-times/include-split.md, Phase 3).
#include "app/AppShared.h"
#include "core/NodeViewport.h"
#include "core/SharedViewportCamera.h"

namespace app
{
extern std::map<int, NodeViewport> gNodeViewports;

extern std::map<int, SharedViewportCamera> gNodeCameras;

extern std::vector<std::map<int, NodeViewport>::node_type> gRetiredViewports;

extern std::map<int, NodeViewport> gPanelViewports;

extern std::map<int, NodeViewport> gProjectorViewports;

}
