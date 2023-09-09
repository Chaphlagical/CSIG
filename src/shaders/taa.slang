#include "common.slangh"
#include "gbuffer.slangh"
#include "random.slangh"
#include "scene.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

struct PushConstant
{
    float4 texel_size;
    float min_blend_factor;
};

[[vk::binding(0, 2)]] Texture2D<float4> Current_Image;
[[vk::binding(0, 3)]] RWTexture2D<float4> Output_Image;
[[vk::binding(1, 3)]] Texture2D<float4> Prev_Image;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

static const float FLT_EPS = 0.00000001;

#define HISTORY_REJECT_NONE 0
#define HISTORY_REJECT_CLAMP 1
#define HISTORY_REJECT_CLIP 2          // [Karis14]
#define HISTORY_REJECT_VARIANCE_CLIP 3 // [Salvi16]

#define HISTORY_RESOLVE_BILINEAR 0
#define HISTORY_RESOLVE_CATMULL_ROM 1

#define COLOR_SPACE_RGB 0
#define COLOR_SPACE_YCOCG 1 // [Karis14]

#define MIN_BLEND_FACTOR 0.05
#define MAX_BLEND_FACTOR 0.12

#define TAA_COLOR_SPACE COLOR_SPACE_YCOCG              // Color space to use for neighborhood clamping
#define TAA_HISTORY_REJECT_METHOD HISTORY_REJECT_CLIP  // Use neighborhood clipping to reject history samples
#define TAA_RESOLVE_METHOD HISTORY_RESOLVE_CATMULL_ROM // History resolve filter
#define TAA_REPROJECT 1                                // Use per pixel velocity to reproject
#define TAA_TONEMAP 0                                  // Tonemap before resolving history to prevent high luminance pixels from overpowering
#define TAA_AABB_ROUNDED 1                             // Use combine 3x3 neighborhood with plus-pattern neighborhood
#define TAA_VELOCITY_CORRECT 0                         // Reduce blend factor when the subpixel motion is high to reduce blur under motion
#define TAA_DEBUG_RED_HISTORY 0
#define TAA_LUMINANCE_WEIGHT 0 // [Lottes]
#define TAA_DILATE_VELOCITY 1

// https://software.intel.com/en-us/node/503873
float3 RGB_to_YCoCg(float3 c)
{
    // Y = R/4 + G/2 + B/4
    // Co = R/2 - B/2
    // Cg = -R/4 + G/2 - B/4
    return float3(
        c.x / 4.0 + c.y / 2.0 + c.z / 4.0,
        c.x / 2.0 - c.z / 2.0,
        -c.x / 4.0 + c.y / 2.0 - c.z / 4.0
	);
}

// https://software.intel.com/en-us/node/503873
float3 YCoCg_to_RGB(float3 c)
{
    // R = Y + Co - Cg
    // G = Y + Cg
    // B = Y - Co - Cg
    return float3(
        c.x + c.y - c.z,
        c.x + c.z,
        c.x - c.y - c.z
	);
}

// Temporal Reprojection in Inside
float4 clip_aabb(float3 aabb_min, float3 aabb_max, float4 p, float4 q)
{
    // note: only clips towards aabb center (but fast!)
    float3 p_clip = 0.5 * (aabb_max + aabb_min);
    float3 e_clip = 0.5 * (aabb_max - aabb_min) + 0.00000001f;

    float4 v_clip = q - float4(p_clip, p.w);
    float3 v_unit = v_clip.xyz / e_clip;
    float3 a_unit = abs(v_unit);
    float ma_unit = max(max(a_unit.x, a_unit.y), a_unit.z);

    if (ma_unit > 1.0)
    {
        return float4(p_clip, p.w) + v_clip / ma_unit;
    }
    else
    {
        return q; // point inside aabb
    }
}

float3 transform_color(float3 color)
{
#if TAA_COLOR_SPACE == COLOR_SPACE_RGB
    return color;
#elif TAA_COLOR_SPACE == COLOR_SPACE_YCOCG
    return RGB_to_YCoCg(color);
#else
#error No color space defined
    return 0;
#endif
}

float3 resolve_color(float3 color)
{
#if TAA_COLOR_SPACE == COLOR_SPACE_RGB
    return color;
#elif TAA_COLOR_SPACE == COLOR_SPACE_YCOCG
    return YCoCg_to_RGB(color);
#else
#error No color space defined
    return 0;
#endif
}

float3 sample_color(Texture2D tex, SamplerState texture_sampler, float2 uv)
{
    return transform_color(tex.SampleLevel(texture_sampler, uv, 0).rgb);
}

// [Jiminez16] - 5 tap cubic texture filter
float3 filter_history(Texture2D tex, SamplerState texture_sampler, float2 uv, float2 dimensions)
{
    float2 position = dimensions * uv;
    float2 centerPosition = floor(position - 0.5) + 0.5;
    float2 f = position - centerPosition;
    float2 f2 = f * f;
    float2 f3 = f * f2;

    const float SHARPNESS = 50.0f;
    float c = SHARPNESS / 100.0;
    float2 w0 = -c * f3 + 2.0 * c * f2 - c * f;
    float2 w1 = (2.0 - c) * f3 - (3.0 - c) * f2 + 1.0;
    float2 w2 = -(2.0 - c) * f3 + (3.0 - 2.0 * c) * f2 + c * f;
    float2 w3 = c * f3 - c * f2;

    float2 w12 = w1 + w2;
    float2 tc12 = push_constant.texel_size.xy * (centerPosition + w2 / w12);
    float3 centerColor = sample_color(tex, texture_sampler, float2(tc12.x, tc12.y));

    float2 tc0 = push_constant.texel_size.xy * (centerPosition - 1.0);
    float2 tc3 = push_constant.texel_size.xy * (centerPosition + 2.0);
    float3 color = sample_color(tex, texture_sampler, float2(tc12.x, tc0.y)) * (w12.x * w0.y) +
                   sample_color(tex, texture_sampler, float2(tc0.x, tc12.y)) * (w0.x * w12.y) +
                   centerColor * (w12.x * w12.y) +
                   sample_color(tex, texture_sampler, float2(tc3.x, tc12.y)) * (w3.x * w12.y) +
                   sample_color(tex, texture_sampler, float2(tc12.x, tc3.y)) * (w12.x * w3.y);
    return color;
}

// The following code is licensed under the MIT license: https://gist.github.com/TheRealMJP/bc503b0b87b643d3505d41eab8b332ae
// Samples a texture with Catmull-Rom filtering, using 9 texture fetches instead of 16.
// See http://vec3.ca/bicubic-filtering-in-fewer-taps/ for more details
float4 sample_texture_catmullrom(in Texture2D<float4> tex, in SamplerState texture_sampler, in float2 uv, in float2 tex_size)
{
    // We're going to sample a a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
    // down the sample location to get the exact center of our "starting" texel. The starting texel will be at
    // location [1, 1] in the grid, where [0, 0] is the top left corner.
    float2 sample_pos = uv * tex_size;
    float2 tex_pos1 = floor(sample_pos - 0.5f) + 0.5f;

    // Compute the fractional offset from our starting texel to our original sample location, which we'll
    // feed into the Catmull-Rom spline function to get our filter weights.
    float2 f = sample_pos - tex_pos1;

    // Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
    // These equations are pre-expanded based on our knowledge of where the texels will be located,
    // which lets us avoid having to evaluate a piece-wise function.
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    // Work out weighting factors and sampling offsets that will let us use bilinear filtering to
    // simultaneously evaluate the middle 2 samples from the 4x4 grid.
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);

    // Compute the final UV coordinates we'll use for sampling the texture
    float2 tex_pos0 = tex_pos1 - 1;
    float2 tex_pos3 = tex_pos1 + 2;
    float2 tex_pos12 = tex_pos1 + offset12;

    tex_pos0 /= tex_size;
    tex_pos3 /= tex_size;
    tex_pos12 /= tex_size;

    float4 result = 0.0f;
    result += tex.SampleLevel(texture_sampler, float2(tex_pos0.x, tex_pos0.y), 0.0f) * w0.x * w0.y;
    result += tex.SampleLevel(texture_sampler, float2(tex_pos12.x, tex_pos0.y), 0.0f) * w12.x * w0.y;
    result += tex.SampleLevel(texture_sampler, float2(tex_pos3.x, tex_pos0.y), 0.0f) * w3.x * w0.y;

    result += tex.SampleLevel(texture_sampler, float2(tex_pos0.x, tex_pos12.y), 0.0f) * w0.x * w12.y;
    result += tex.SampleLevel(texture_sampler, float2(tex_pos12.x, tex_pos12.y), 0.0f) * w12.x * w12.y;
    result += tex.SampleLevel(texture_sampler, float2(tex_pos3.x, tex_pos12.y), 0.0f) * w3.x * w12.y;

    result += tex.SampleLevel(texture_sampler, float2(tex_pos0.x, tex_pos3.y), 0.0f) * w0.x * w3.y;
    result += tex.SampleLevel(texture_sampler, float2(tex_pos12.x, tex_pos3.y), 0.0f) * w12.x * w3.y;
    result += tex.SampleLevel(texture_sampler, float2(tex_pos3.x, tex_pos3.y), 0.0f) * w3.x * w3.y;

    return float4(transform_color(result.rgb), result.a);
}

float3 reinhard(float3 x)
{
    return x / (1.0 + x);
}

float3 inv_reinhard(float3 x)
{
    return x / (1.0 - x);
}

#define THREAD_GROUP_ROW_SIZE 8
#define THREAD_GROUP_SIZE (THREAD_GROUP_ROW_SIZE * THREAD_GROUP_ROW_SIZE)
#define GSM_ROW_SIZE (1 + THREAD_GROUP_ROW_SIZE + 1)
#define GSM_SIZE (GSM_ROW_SIZE * GSM_ROW_SIZE)

groupshared float3 shared_colors[GSM_SIZE];
groupshared float shared_depths[GSM_SIZE];

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    const float2 dxdy = push_constant.texel_size.xy;
    uint2 pixelIndex = param.DispatchThreadID.xy;
    float2 uv = dxdy * ((float2)pixelIndex + 0.5f);
    float2 dimensions;
    Current_Image.GetDimensions(dimensions.x, dimensions.y);

    int gsLocation = param.GroupThreadID.x + param.GroupThreadID.y * GSM_ROW_SIZE + GSM_ROW_SIZE + 1;
    int gsPrefetchLocation0 = param.GroupThreadID.x + param.GroupThreadID.y * THREAD_GROUP_ROW_SIZE;
    int gsPrefetchLocation1 = gsPrefetchLocation0 + GSM_SIZE - THREAD_GROUP_SIZE;
    int2 prefetchLocation0 = int2(pixelIndex.x & -8, pixelIndex.y & -8) - 1 + int2(gsPrefetchLocation0 % 10, gsPrefetchLocation0 / 10);
    int2 prefetchLocation1 = int2(pixelIndex.x & -8, pixelIndex.y & -8) - 1 + int2(gsPrefetchLocation1 % 10, gsPrefetchLocation1 / 10);

    shared_colors[gsPrefetchLocation0] = transform_color(Current_Image[prefetchLocation0].rgb);
    shared_colors[gsPrefetchLocation1] = transform_color(Current_Image[prefetchLocation1].rgb);
    shared_depths[gsPrefetchLocation0] = DepthBuffer[prefetchLocation0].r;
    shared_depths[gsPrefetchLocation1] = DepthBuffer[prefetchLocation1].r;

    GroupMemoryBarrierWithGroupSync();

    float3 cc = shared_colors[gsLocation];
    float3 currColor = cc;

#if TAA_HISTORY_REJECT_METHOD != HISTORY_REJECT_NONE
    // Get a 3x3 neighborhood to clip/clamp against
    float3 lt = shared_colors[gsLocation - 11];
    float3 ct = shared_colors[gsLocation - 10];
    float3 rt = shared_colors[gsLocation - 9];
    float3 lc = shared_colors[gsLocation - 1];
    float3 rc = shared_colors[gsLocation + 1];
    float3 lb = shared_colors[gsLocation + 9];
    float3 cb = shared_colors[gsLocation + 10];
    float3 rb = shared_colors[gsLocation + 11];

#if TAA_HISTORY_REJECT_METHOD == HISTORY_REJECT_CLAMP || TAA_HISTORY_REJECT_METHOD == HISTORY_REJECT_CLIP
    float3 aabb_min = min(lt, min(ct, min(rt, min(lc, min(cc, min(rc, min(lb, min(cb, rb))))))));
    float3 aabb_max = max(lt, max(ct, max(rt, max(lc, max(cc, max(rc, max(lb, max(cb, rb))))))));
    float3 aabb_avg = (lt + ct + rt + lc + cc + rc + lb + cb + rb) / 9.0f;
#if TAA_AABB_ROUNDED
    //[Karis14] - Average 3x3 neighborhoord with 5 sample plus pattern neighborhood to remove 'filtered' look
    float3 aabb_min2 = min(min(min(min(lc, cc), ct), rc), cb);
    float3 aabb_max2 = max(max(max(max(lc, cc), ct), rc), cb);
    float3 aabb_avg2 = (lc + cc + ct + rc + cb) / 5.0f;
    aabb_min = (aabb_min + aabb_min2) * 0.5f;
    aabb_max = (aabb_max + aabb_max2) * 0.5f;
    aabb_avg = (aabb_avg + aabb_avg2) * 0.5f;
#endif // TAA_AABB_ROUNDED

#elif TAA_HISTORY_REJECT_METHOD == HISTORY_REJECT_VARIANCE_CLIP
    // [Salvi16] - Use first and second moment to clip history color
    float3 m1 = lt + ct + rt + lc + cc + rc + lb + cb + rb;
    float3 m2 = lt * lt + ct * ct + rt * rt + lc * lc + cc * cc + rc * rc + lb * lb + cb * cb + rb * rb;
    float3 mu = m1 / 9.0f;
    float3 sigma = sqrt(m2 / 9.0f - mu * mu);
    const float gamma = 1.0f;
    float3 aabb_min = mu - gamma * sigma;
    float3 aabb_max = mu + gamma * sigma;
    float3 aabb_avg = mu;
#endif // TAA_HISTORY_REJECT_METHOD == HISTORY_REJECT_CLAMP || TAA_HISTORY_REJECT_METHOD == HISTORY_REJECT_CLIP

#endif // TAA_HISTORY_REJECT_METHOD != HISTORY_REJECT_NONE

    float2 uv_reproj = uv;

#if TAA_REPROJECT
    float depth = DepthBuffer.SampleLevel(Samplers[int(SamplerType::Nearest)], uv_reproj, 0).r;

#if TAA_DILATE_VELOCITY
    // [Karis14] - Use closest pixel to move edge along
    const float crossDilation = 2;
    float4 cross_depth = float4(
        shared_depths[gsLocation - 11],
        shared_depths[gsLocation - 9],
        shared_depths[gsLocation + 9],
        shared_depths[gsLocation + 11]
	);
    float3 min_offset = float3(-1.0f, -1.0f, cross_depth.x);
    if (cross_depth.y > min_offset.z)
    {
        min_offset = float3(1.0f, -1.0f, cross_depth.y);
    }
    if (cross_depth.z > min_offset.z)
    {
        min_offset = float3(-1.0f, 1.0f, cross_depth.z);
    }
    if (cross_depth.w > min_offset.z)
    {
        min_offset = float3(1.0f, 1.0f, cross_depth.w);
    }
    float2 velocity = GBufferB.SampleLevel(Samplers[int(SamplerType::Nearest)], uv_reproj + min_offset.xy * dxdy, 0).zw;
#else
    float2 velocity = GBufferB.SampleLevel(Samplers[int(SamplerType::Nearest)], uv_reproj, 0).zw;
#endif // TAA_DILATE_VELOCITY
    uv_reproj = uv + velocity;
#endif // TAA_REPROJECT

#if TAA_RESOLVE_METHOD == HISTORY_RESOLVE_CATMULL_ROM
    // [Karis14] Cubic filter to avoid blurry result from billinear filter
    float3 prev_color = sample_texture_catmullrom(Prev_Image, Samplers[int(SamplerType::Linear)], uv_reproj, dimensions).rgb;
#elif TAA_RESOLVE_METHOD == HISTORY_RESOLVE_BILINEAR
    float3 prev_color = sample_color(Prev_Image, Samplers[int(SamplerType::Linear)], uv_reproj);
#else
#error No history resolve method specified
    float3 prev_color = 0;
#endif // TAA_RESOLVE_METHOD

#if TAA_DEBUG_RED_HISTORY
    // DEBUG: Use red history to debug how correct neighborhood clamp is
    prev_color = transform_color(float3(1, 0, 0));
#endif // TAA_DEBUG_RED_HISTORY

#if TAA_HISTORY_REJECT_METHOD == HISTORY_REJECT_CLAMP
    prev_color = clamp(prev_color, aabb_min, aabb_max);
#elif TAA_HISTORY_REJECT_METHOD == HISTORY_REJECT_CLIP
    // [Karis2014] - Clip instead of clamp
    prev_color = clip_aabb(aabb_min, aabb_max, float4(aabb_avg, 1), float4(prev_color, 1)).xyz;
#elif TAA_HISTORY_REJECT_METHOD == HISTORY_REJECT_VARIANCE_CLIP
    // [Salvi16]
    prev_color = clip_aabb(aabb_min, aabb_max, float4(aabb_avg, 1), float4(prev_color, 1)).xyz;
#endif // TAA_HISTORY_REJECT_METHOD

    float blend_factor = MIN_BLEND_FACTOR;

#if TAA_VELOCITY_CORRECT
    // [Xu16] Reduce blend factor when the motion is more subpixel
    float subpixel_correction = frac(max(abs(velocity.x) * dimensions.x, abs(velocity.y) * dimensions.y)) * 0.5f;
    blend_factor = saturate(lerp(blend_factor, 0.8f, subpixel_correction));
#endif // TAA_VELOCITY_CORRECT

#if TAA_LUMINANCE_WEIGHT
    // [Lottes] Feedback weight from unbiased luminance diff
#if TAA_COLOR_SPACE == COLOR_SPACE_RGB
    float lum0 = luminance(currColor);
    float lum1 = luminance(prev_color);
#else
    float lum0 = currColor.x;
    float lum1 = prev_color.x;
#endif
    float unbiased_diff = abs(lum0 - lum1) / max(lum0, max(lum1, 0.2));
    float unbiased_weight = unbiased_diff;
    float unbiased_weight_sqr = unbiased_weight * unbiased_weight;
    blend_factor = lerp(MIN_BLEND_FACTOR, MAX_BLEND_FACTOR, blend_factor);
#endif

#if TAA_TONEMAP
    currColor = reinhard(currColor);
    prev_color = reinhard(prev_color);
#endif

    blend_factor = max(push_constant.min_blend_factor, blend_factor);

    if (any(uv_reproj < 0) || any(uv_reproj > 1))
        blend_factor = 1;

    currColor = lerp(prev_color, currColor, blend_factor);

#if TAA_TONEMAP
    currColor = inv_reinhard(currColor);
#endif

    currColor = resolve_color(currColor);

    Output_Image[pixelIndex] = float4(currColor, 1);
}
