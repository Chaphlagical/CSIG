#include "scene.slangh"
#include "gbuffer.slangh"
#include "random.slangh"
#include "common.slangh"

#define NUM_THREAD_X 8
#define NUM_THREAD_Y 4
#define SAMPLER_WHITE_NOISE 0
#define SAMPLER_BLUE_NOISE_DISTRIBUTION 1

struct PushConstant
{
    float ray_length;
	float bias;
	int gbuffer_mip;
};

[[vk::binding(0, 2)]] RWTexture2D<uint> RaytracedImage;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float2 random_sample(int2 coord, uint num_frames)
{
    return float2(sample_blue_noise(coord, int(num_frames), 0, SobelSequence, ScramblingRankingTile[0]),
                  sample_blue_noise(coord, int(num_frames), 1, SobelSequence, ScramblingRankingTile[0]));
}

float3x3 make_rotation_matrix(float3 z)
{
    const float3 ref = abs(dot(z, float3(0, 1, 0))) > 0.99f ? float3(0, 0, 1) : float3(0, 1, 0);

    const float3 x = normalize(cross(ref, z));
    const float3 y = cross(z, x);

    return float3x3(x, y, z);
}

float3 sample_cosine_lobe(in float3 n, in float2 r)
{
    float2 rand_sample = max(float2(0.00001f), r);

    const float phi = 2.0f * PI * rand_sample.y;

    const float cos_theta = sqrt(rand_sample.x);
    const float sin_theta = sqrt(1 - rand_sample.x);

    float3 t = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    return normalize(mul(transpose(make_rotation_matrix(n)), t));
}

bool any_hit(float3 org, float3 dir, float max_dist)
{
    RayDesc ray;
    ray.Origin = org;
    ray.TMin = 0.001;
    ray.Direction = dir;
    ray.TMax = max_dist;
    RayQuery<RAY_FLAG_NONE> q;

    q.TraceRayInline(
        TLAS,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE,
        0xff,
        ray);

    while(q.Proceed()) {};

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        return true;
    }
    return false;
}

groupshared uint shared_ao;

[shader("compute")]
[numthreads(NUM_THREAD_X, NUM_THREAD_Y, 1)]
void main(CSParam param)
{
    if (param.GroupIndex == 0)
        shared_ao = 0;

    GroupMemoryBarrierWithGroupSync();

    const int2 size = texture_size(GBufferA, push_constant.gbuffer_mip);
    const int2 current_coord = int2(param.DispatchThreadID.xy);
    const float2 pixel_center = float2(current_coord) + float2(0.5);
    const float2 tex_coord = pixel_center / float2(size);

    float depth = DepthBuffer.Load(int3(current_coord, push_constant.gbuffer_mip)).r;

    uint result = 0;

    if (depth != 1.0f)
    {
        float3 world_pos = world_position_from_depth(tex_coord, depth, ViewBuffer.view_projection_inv);
        float3 normal = octohedral_to_direction(GBufferB.Load(int3(current_coord, push_constant.gbuffer_mip)).rg);
        float3 ray_origin = world_pos + normal * push_constant.bias;
        float2 rnd_sample = random_sample(current_coord, uint(ViewBuffer.cam_pos.w));
        float3 sample_direction = sample_cosine_lobe(normal, rnd_sample);
        result = uint(!any_hit(ray_origin, sample_direction, push_constant.ray_length));
    }

    InterlockedOr(shared_ao, result << param.GroupIndex);

    GroupMemoryBarrierWithGroupSync();

    if (param.GroupIndex == 0)
        RaytracedImage[int2(param.GroupID.xy)] = shared_ao;
}