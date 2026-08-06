#ifndef SOC_PIPELINE_H_INCLUDED
#define SOC_PIPELINE_H_INCLUDED

#include <soc/soc.h>

soc_result soc_occlusion_build_internal(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
);

/* Dense backend retained as the high-triangle-count path and test oracle. */
soc_result soc_occlusion_build_dense_internal(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
);

/* Forced masked backend used by differential and parallel tests. */
soc_result soc_occlusion_build_masked_internal(
    soc_context* context,
    const soc_occlusion_build_desc* desc,
    soc_snapshot** out_snapshot
);

#endif
