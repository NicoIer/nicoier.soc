using System.Runtime.InteropServices;

namespace soc
{
    internal partial struct soc_context
    {
    }

    internal partial struct soc_mesh
    {
    }

    internal partial struct soc_vector2
    {
        public float x;

        public float y;
    }

    internal partial struct soc_vector3
    {
        public float x;

        public float y;

        public float z;
    }

    internal partial struct soc_vector4
    {
        public float x;

        public float y;

        public float z;

        public float w;
    }

    internal partial struct soc_mat4
    {
        public soc_vector4 col0;

        public soc_vector4 col1;

        public soc_vector4 col2;

        public soc_vector4 col3;
    }

    internal partial struct soc_aabb
    {
        public soc_vector3 min;

        public soc_vector3 max;
    }

    internal partial struct soc_config
    {
        [NativeTypeName("uint32_t")]
        public uint struct_size;

        [NativeTypeName("uint32_t")]
        public uint width;

        [NativeTypeName("uint32_t")]
        public uint height;

        [NativeTypeName("uint32_t")]
        public uint worker_count;

        [NativeTypeName("uint32_t")]
        public uint flags;
    }

    internal partial struct soc_frame_desc
    {
        [NativeTypeName("uint32_t")]
        public uint struct_size;

        public soc_mat4 clip_from_world;

        [NativeTypeName("soc_clip_depth_range")]
        public uint clip_depth_range;

        [NativeTypeName("soc_depth_direction")]
        public uint depth_direction;

        [NativeTypeName("soc_front_face")]
        public uint front_face;

        [NativeTypeName("uint32_t")]
        public uint flags;
    }

    internal unsafe partial struct soc_mesh_desc
    {
        [NativeTypeName("uint32_t")]
        public uint struct_size;

        [NativeTypeName("uint32_t")]
        public uint flags;

        [NativeTypeName("const void *")]
        public void* vertices;

        [NativeTypeName("const void *")]
        public void* indices;

        [NativeTypeName("uint32_t")]
        public uint vertex_count;

        [NativeTypeName("uint32_t")]
        public uint vertex_stride;

        [NativeTypeName("uint32_t")]
        public uint position_offset;

        [NativeTypeName("uint32_t")]
        public uint index_count;

        [NativeTypeName("soc_index_type")]
        public uint index_type;
    }

    internal partial struct soc_stats
    {
        [NativeTypeName("uint32_t")]
        public uint struct_size;

        [NativeTypeName("uint32_t")]
        public uint hiz_level_count;

        [NativeTypeName("uint64_t")]
        public ulong input_triangle_count;

        [NativeTypeName("uint64_t")]
        public ulong clipped_triangle_count;

        [NativeTypeName("uint64_t")]
        public ulong rasterized_triangle_count;

        [NativeTypeName("uint64_t")]
        public ulong tested_aabb_count;

        [NativeTypeName("uint64_t")]
        public ulong occluded_aabb_count;
    }

    internal partial struct soc_hiz_level_info
    {
        [NativeTypeName("uint32_t")]
        public uint struct_size;

        [NativeTypeName("uint32_t")]
        public uint level;

        [NativeTypeName("uint32_t")]
        public uint width;

        [NativeTypeName("uint32_t")]
        public uint height;

        [NativeTypeName("uint64_t")]
        public ulong required_element_count;
    }

    internal static unsafe partial class Methods
    {
        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("uint32_t")]
        public static extern uint soc_get_abi_version();

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_context_create([NativeTypeName("const soc_config *")] soc_config* config, soc_context** out_context);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        public static extern void soc_context_destroy(soc_context* context);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_context_resize(soc_context* context, [NativeTypeName("uint32_t")] uint width, [NativeTypeName("uint32_t")] uint height);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_mesh_create(soc_context* context, [NativeTypeName("const soc_mesh_desc *")] soc_mesh_desc* desc, soc_mesh** out_mesh);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_mesh_destroy(soc_mesh* mesh);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_frame_begin(soc_context* context, [NativeTypeName("const soc_frame_desc *")] soc_frame_desc* desc);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_occluders_submit(soc_context* context, [NativeTypeName("const soc_mesh *")] soc_mesh* mesh, [NativeTypeName("const soc_mat4 *")] soc_mat4* object_to_world, [NativeTypeName("uint32_t")] uint instance_count);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_occluders_finish(soc_context* context);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_visibility_test_aabbs(soc_context* context, [NativeTypeName("const soc_aabb *")] soc_aabb* world_bounds, [NativeTypeName("uint32_t")] uint bounds_count, [NativeTypeName("soc_visibility *")] byte* out_visibility);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_frame_end(soc_context* context);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_context_get_stats([NativeTypeName("const soc_context *")] soc_context* context, soc_stats* out_stats);

        [DllImport("libsoc", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        [return: NativeTypeName("soc_result")]
        public static extern int soc_hiz_level_query([NativeTypeName("const soc_context *")] soc_context* context, [NativeTypeName("uint32_t")] uint level, soc_hiz_level_info* out_info, float* out_depth, [NativeTypeName("uint64_t")] ulong out_depth_count);

        [NativeTypeName("#define SOC_ABI_VERSION_MAJOR 1u")]
        public const uint SOC_ABI_VERSION_MAJOR = 1U;

        [NativeTypeName("#define SOC_ABI_VERSION_MINOR 0u")]
        public const uint SOC_ABI_VERSION_MINOR = 0U;

        [NativeTypeName("#define SOC_ABI_VERSION ((SOC_ABI_VERSION_MAJOR << 16u) | SOC_ABI_VERSION_MINOR)")]
        public const uint SOC_ABI_VERSION = ((1U << 16) | 0U);

        [NativeTypeName("#define SOC_FALSE ((soc_bool)0u)")]
        public const byte SOC_FALSE = ((byte)(0U));

        [NativeTypeName("#define SOC_TRUE ((soc_bool)1u)")]
        public const byte SOC_TRUE = ((byte)(1U));

        [NativeTypeName("#define SOC_RESULT_OK ((soc_result)0)")]
        public const int SOC_RESULT_OK = ((int)(0));

        [NativeTypeName("#define SOC_RESULT_INVALID_ARGUMENT ((soc_result)-1)")]
        public const int SOC_RESULT_INVALID_ARGUMENT = ((int)(-1));

        [NativeTypeName("#define SOC_RESULT_OUT_OF_MEMORY ((soc_result)-2)")]
        public const int SOC_RESULT_OUT_OF_MEMORY = ((int)(-2));

        [NativeTypeName("#define SOC_RESULT_UNSUPPORTED ((soc_result)-3)")]
        public const int SOC_RESULT_UNSUPPORTED = ((int)(-3));

        [NativeTypeName("#define SOC_RESULT_INTERNAL_ERROR ((soc_result)-4)")]
        public const int SOC_RESULT_INTERNAL_ERROR = ((int)(-4));

        [NativeTypeName("#define SOC_RESULT_INVALID_STATE ((soc_result)-5)")]
        public const int SOC_RESULT_INVALID_STATE = ((int)(-5));

        [NativeTypeName("#define SOC_RESULT_BUFFER_TOO_SMALL ((soc_result)-6)")]
        public const int SOC_RESULT_BUFFER_TOO_SMALL = ((int)(-6));

        [NativeTypeName("#define SOC_VISIBILITY_UNKNOWN ((soc_visibility)0u)")]
        public const byte SOC_VISIBILITY_UNKNOWN = ((byte)(0U));

        [NativeTypeName("#define SOC_VISIBILITY_VISIBLE ((soc_visibility)1u)")]
        public const byte SOC_VISIBILITY_VISIBLE = ((byte)(1U));

        [NativeTypeName("#define SOC_VISIBILITY_OCCLUDED ((soc_visibility)2u)")]
        public const byte SOC_VISIBILITY_OCCLUDED = ((byte)(2U));

        [NativeTypeName("#define SOC_INDEX_UINT16 ((soc_index_type)0u)")]
        public const uint SOC_INDEX_UINT16 = ((uint)(0U));

        [NativeTypeName("#define SOC_INDEX_UINT32 ((soc_index_type)1u)")]
        public const uint SOC_INDEX_UINT32 = ((uint)(1U));

        [NativeTypeName("#define SOC_CLIP_DEPTH_ZERO_TO_ONE ((soc_clip_depth_range)0u)")]
        public const uint SOC_CLIP_DEPTH_ZERO_TO_ONE = ((uint)(0U));

        [NativeTypeName("#define SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE ((soc_clip_depth_range)1u)")]
        public const uint SOC_CLIP_DEPTH_NEGATIVE_ONE_TO_ONE = ((uint)(1U));

        [NativeTypeName("#define SOC_DEPTH_FORWARD ((soc_depth_direction)0u)")]
        public const uint SOC_DEPTH_FORWARD = ((uint)(0U));

        [NativeTypeName("#define SOC_DEPTH_REVERSED ((soc_depth_direction)1u)")]
        public const uint SOC_DEPTH_REVERSED = ((uint)(1U));

        [NativeTypeName("#define SOC_FRONT_FACE_CCW ((soc_front_face)0u)")]
        public const uint SOC_FRONT_FACE_CCW = ((uint)(0U));

        [NativeTypeName("#define SOC_FRONT_FACE_CW ((soc_front_face)1u)")]
        public const uint SOC_FRONT_FACE_CW = ((uint)(1U));

        [NativeTypeName("#define SOC_CONFIG_FLAG_NONE 0u")]
        public const uint SOC_CONFIG_FLAG_NONE = 0U;

        [NativeTypeName("#define SOC_CONFIG_SIZE_V1 ((uint32_t)(offsetof(soc_config, flags) + sizeof(uint32_t)))")]
        public static readonly uint SOC_CONFIG_SIZE_V1 = ((uint)(Marshal.OffsetOf<soc_config>("flags") + 4));

        [NativeTypeName("#define SOC_FRAME_FLAG_NONE 0u")]
        public const uint SOC_FRAME_FLAG_NONE = 0U;

        [NativeTypeName("#define SOC_FRAME_DESC_SIZE_V1 ((uint32_t)(offsetof(soc_frame_desc, flags) + sizeof(uint32_t)))")]
        public static readonly uint SOC_FRAME_DESC_SIZE_V1 = ((uint)(Marshal.OffsetOf<soc_frame_desc>("flags") + 4));

        [NativeTypeName("#define SOC_MESH_FLAG_NONE 0u")]
        public const uint SOC_MESH_FLAG_NONE = 0U;

        [NativeTypeName("#define SOC_MESH_FLAG_TWO_SIDED (1u << 0u)")]
        public const uint SOC_MESH_FLAG_TWO_SIDED = (1U << 0);

        [NativeTypeName("#define SOC_MESH_DESC_SIZE_V1 ((uint32_t)(offsetof(soc_mesh_desc, index_type) + sizeof(soc_index_type)))")]
        public static readonly uint SOC_MESH_DESC_SIZE_V1 = ((uint)(Marshal.OffsetOf<soc_mesh_desc>("index_type") + 4));

        [NativeTypeName("#define SOC_STATS_SIZE_V1 ((uint32_t)(offsetof(soc_stats, occluded_aabb_count) + sizeof(uint64_t)))")]
        public static readonly uint SOC_STATS_SIZE_V1 = ((uint)(Marshal.OffsetOf<soc_stats>("occluded_aabb_count") + 8));

        [NativeTypeName("#define SOC_HIZ_LEVEL_INFO_SIZE_V1 ((uint32_t)(offsetof(soc_hiz_level_info, required_element_count) + \\\n        sizeof(uint64_t)))")]
        public static readonly uint SOC_HIZ_LEVEL_INFO_SIZE_V1 = ((uint)(Marshal.OffsetOf<soc_hiz_level_info>("required_element_count") + 8));
    }
}
