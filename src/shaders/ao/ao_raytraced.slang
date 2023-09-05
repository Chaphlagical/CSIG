#include "scene.slangh"
#include "common.slangh"
#include "gbuffer.slangh"
#include "random.slangh"

struct PushConstant
{
    float ray_length;
	float bias;
	int gbuffer_mip;
};

[[vk::binding(0, 2)]] RWTexture2D<uint> RayTracedImage;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

float3 sample_cosine_lobe(float3 n, float2 rnd)
{
	rnd = max(rnd, float2(0.00001));
	const float phi = 2.0 * PI * rnd.y;
	const float cos_theta = sqrt(rnd.x);
	const float sin_theta = sqrt(1.0 - rnd.x);
	float3 t = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	return local_to_world(n, t);
}

bool any_hit(float3 org, float3 dir, float max_dist)
{
    RayDesc ray;
    ray.Origin = org;
    ray.TMin = 0.0;
    ray.Direction = dir;
    ray.TMax = max_dist;
    RayQuery<RAY_FLAG_NONE> q;

    q.TraceRayInline(
        TLAS,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xff,
        ray);
    q.Proceed();

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        return true;
    }
    return false;
}

groupshared uint shared_ao;

[shader("compute")]
[numthreads(8, 4, 1)]
void main(CSParam param)
{
    if(param.GroupIndex == 0)
	{
		shared_ao = 0;
	}

    GroupMemoryBarrierWithGroupSync();

    const int2 size = texture_size(GBufferB, push_constant.gbuffer_mip);
	const int2 coord = int2(param.DispatchThreadID.xy);
	const float2 pixel = float2(coord) + float2(0.5);
	const float2 uv = pixel / float2(size);

    if(coord.x >= size.x || coord.y >= size.y)
    {
        return;
    }

	uint seed = tea(size.x * param.DispatchThreadID.y + param.DispatchThreadID.x, uint(ViewBuffer.cam_pos.w));
    float depth = DepthBuffer.Load(int3(coord, push_constant.gbuffer_mip)).r;

    uint result = 0;

	if(depth != 1.0)
	{
		float3 world_pos = world_position_from_depth(uv, depth, ViewBuffer.view_projection_inv);
		float3 normal = octohedral_to_direction(GBufferB.Load(int3(coord, push_constant.gbuffer_mip)).rg);
		float3 dir = sample_cosine_lobe(normal, rand2(seed));
		float3 org = world_pos + push_constant.bias * normal;
		result = uint(!any_hit(org, dir, push_constant.ray_length));
	}

    InterlockedOr(shared_ao, result << param.GroupIndex);

    GroupMemoryBarrierWithGroupSync();

    if (param.GroupIndex == 0)
	{
        RayTracedImage[param.GroupID.xy] = shared_ao;
	}
}