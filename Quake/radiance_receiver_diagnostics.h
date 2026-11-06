#ifndef RADIANCE_RECEIVER_DIAGNOSTICS_H
#define RADIANCE_RECEIVER_DIAGNOSTICS_H
#include "radiance_receiver_record.h"

/* Optional opaque-world coverage estimates. These describe the prepass and
 * geometry-guided fast resolve, not actual forward-fragment execution. */
enum {
    RC_DIAG_EMPTY_BLOCKS, RC_DIAG_REUSABLE_BLOCKS, RC_DIAG_MIXED_BLOCKS,
    RC_DIAG_RAW_GI_VALID, RC_DIAG_SELECTED_GI_VALID, RC_DIAG_COVERED_PIXELS,
    RC_DIAG_GI_REUSE, RC_DIAG_GI_FALLBACK, RC_DIAG_DIRECT_REUSE, RC_DIAG_DIRECT_FALLBACK,
    RC_DIAG_INVALID_GEOMETRY, RC_DIAG_TOTAL_BLOCKS, RC_DIAG_COUNTER_COUNT=16
};
typedef struct {
    uint32_t count[RC_DIAG_COUNTER_COUNT];
    uint64_t frame_serial;
    uint32_t width,height;
    float divisor;
    bool use_resolved;
} rc_receiver_diagnostic_stats_t;
typedef struct {
    VkDevice device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t *mapped;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    const rc_receiver_targets_t *targets; /* borrowed; recreate if views change */
    VkSampler nearest;
    VkFence fence; /* borrowed; Collect before its next reset */
    bool recorded;
    rc_receiver_diagnostic_stats_t metadata;
} rc_receiver_diagnostics_t;

/* Lazy, transactional creation; owns its embedded shader module temporarily.
 * Failure leaves the old object untouched. This optional path must never gate
 * lighting. Targets and sampler remain owned by the receiver slot. */
bool RCReceiverDiagnostics_Create(rc_receiver_diagnostics_t *out,VkDevice device,
    const VkPhysicalDeviceMemoryProperties *memory,const rc_receiver_targets_t *targets,
    VkSampler nearest,VkPipelineCache cache);
/* enabled=false succeeds without recording any GPU commands. Otherwise call
 * after the receiver/filter dispatch, outside a render pass, on an idle slot. */
bool RCReceiverDiagnostics_Record(rc_receiver_diagnostics_t *diagnostics,VkCommandBuffer command,
    uint64_t frame_serial,const rc_receiver_params_t *params,float probe_spacing,bool use_resolved,bool enabled);
bool RCReceiverDiagnostics_Submitted(rc_receiver_diagnostics_t *diagnostics,VkFence fence);
/* No waits: returns false if there is no completed submitted result. Collect
 * after the frame fence signals and before that fence is reset/reused. */
bool RCReceiverDiagnostics_Collect(rc_receiver_diagnostics_t *diagnostics,rc_receiver_diagnostic_stats_t *stats);
/* Only for command buffers that were recorded but will not be submitted. */
void RCReceiverDiagnostics_Discard(rc_receiver_diagnostics_t *diagnostics);
/* Refuses to destroy recorded/in-flight work. Caller normally already waited
 * for the owning slot fence/device; no device-wide wait is introduced here. */
bool RCReceiverDiagnostics_Destroy(rc_receiver_diagnostics_t *diagnostics);
#endif
