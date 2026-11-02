#ifndef QUIVER_OCTREE_RENDER_H
#define QUIVER_OCTREE_RENDER_H

// Register rendering controls before startup configs are executed.
void  R_OctreeInit ();
void  R_OctreeStats ();
// Prepare is called on the world-owning thread before rendering tasks start.
void  R_OctreePrepare ();
// Called once from GL_BeginRenderingTask, after waiting for the frame fence.
void  R_OctreeFrameReady ();
// Called only by world rendering task zero, after GL_BeginRenderingTask.
void  R_OctreeDraw (cb_context_t *cbx);
// Replay this frame's submitted terrain in the engine's late showtris pass.
void  R_OctreeDrawShowTris (cb_context_t *cbx);
// The caller must join rendering tasks and wait for the Vulkan device to idle.
void  R_OctreeReset ();

#endif
