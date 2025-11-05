public module aperture;

internal struct ApertureInternalUBO {
internal float3 camera_Position; // 0
internal int world_Time; // 12
internal float3 temporal_Position; // 16
internal int camera_Fluid; // 28
internal float3 celestial_Position; // 32
internal int world_RenderDistance; // 44
internal float3 celestial_SunPosition; // 48
internal float world_FogStart; // 60
internal float3 celestial_UpPosition; // 64
internal float world_FogEnd; // 76
internal float3 celestial_MoonPosition; // 80
internal int lights_PointLightCount; // 92
internal int3 internal_ChunkDiameter; // 96
internal float lights_NearPlane; // 108
internal float4x4 camera_Projection; // 112
internal float4x4 lights_Projection; // 176
internal float4x4 camera_ProjectionInv; // 240
internal float4x4 camera_View; // 304
internal float4x4 camera_ViewInv; // 368
internal float4x4 temporal_Projection; // 432
internal float4x4 temporal_ProjectionInv; // 496
internal float4x4 temporal_View; // 560
internal float4x4 temporal_ViewInv; // 624
internal float4x4 celestial_View; // 688
internal float4x4 celestial_ViewInv; // 752
internal float4x4[8] celestial_Projection; // 816
internal float4x4[8] celestial_ProjectionInv; // 1328
internal float4 world_FogColor; // 1840
internal float4 world_SkyColor; // 1856
internal int2 game_ScreenSize; // 1872
internal float lights_FarPlane; // 1880
internal float camera_Near; // 1884
internal float camera_Far; // 1888
internal float world_RainStrength; // 1892
internal float celestial_Angle; // 1896
internal int timing_FrameCounter; // 1900
internal float timing_TimeElapsed; // 1904
internal float timing_LastFrameTime; // 1908
internal float game_Brightness; // 1912
internal int game_MainHand; // 1916
internal int game_OffHand; // 1920
};

internal ConstantBuffer<ApertureInternalUBO> ApertureData;

internal StructuredBuffer<uint4> iris_BlockData;

public struct ChunkSection {
    static ChunkSection EMPTY = ChunkSection(int4(0));
    public int4 chunkData;
    internal int4 blocks[16 * 16 * 16];
}

internal StructuredBuffer<ChunkSection> iris_VoxelData;
internal StructuredBuffer<uint> iris_VoxelIndices;

internal StructuredBuffer<PointLight> iris_PointLightBuffer;

struct CameraData {
    property float4x4 projection { get { return ApertureData.camera_Projection; } }
    property float4x4 projectionInv { get { return ApertureData.camera_ProjectionInv; } }
    property float4x4 view { get { return ApertureData.camera_View; } }
    property float4x4 viewInv { get { return ApertureData.camera_ViewInv; } }
    property float3 position { get { return ApertureData.camera_Position; } }
    property float near { get { return ApertureData.camera_Near; } }
    property float far { get { return ApertureData.camera_Far; } }
    property int fluid { get { return ApertureData.camera_Fluid; } }
}

struct TemporalData {
    property float3 position { get { return ApertureData.temporal_Position; } }
    property float4x4 projection { get { return ApertureData.temporal_Projection; } }
    property float4x4 view { get { return ApertureData.temporal_View; } }
    property float4x4 projectionInv { get { return ApertureData.temporal_ProjectionInv; } }
    property float4x4 viewInv { get { return ApertureData.temporal_ViewInv; } }
}

struct ProjectionReceiver {
    __subscript(int index)->float4x4 {
        get {
            return ApertureData.celestial_Projection[index];
        }
    }
}

struct ProjectionInvReceiver {
    __subscript(int index)->float4x4 {
        get {
            return ApertureData.celestial_ProjectionInv[index];
        }
    }
}

struct CelestialData {
    property ProjectionReceiver projection { get { return ProjectionReceiver(); } }
    property ProjectionInvReceiver projectionInv { get { return ProjectionInvReceiver(); } }
    property float4x4 view { get { return ApertureData.celestial_View; } }
    property float4x4 viewInv { get { return ApertureData.celestial_ViewInv; } }
    property float3 position { get { return ApertureData.celestial_Position; } }
    property float3 sunPosition { get { return ApertureData.celestial_SunPosition; } }
    property float3 moonPosition { get { return ApertureData.celestial_MoonPosition; } }
    property float3 upPosition { get { return ApertureData.celestial_UpPosition; } }
    property float angle { get { return ApertureData.celestial_Angle; } }
}

struct GameData {
    property float brightness { get { return ApertureData.game_Brightness; } }
    property int2 screenSize { get { return ApertureData.game_ScreenSize; } }
    property int mainHand { get { return ApertureData.game_MainHand; } }
    property int offHand { get { return ApertureData.game_OffHand; } }
}

struct TimeData {
    property uint frameCounter { get { return ApertureData.timing_FrameCounter; } }
    property float timeElapsed { get { return ApertureData.timing_TimeElapsed; } }
    property float lastFrameTime { get { return ApertureData.timing_LastFrameTime; } }
}

struct BlockBuffer {
    __subscript(uint id)->BlockData {
        get {
            return BlockData(iris_BlockData[id]);
        }
    }
}

internal float3 glMod(int3 x, int3 m) {
    __intrinsic_asm "mod";
}

internal int3 ap_imod(int3 x, int3 m) {
    int3 r = int3(glMod(x, m));
    return r + m * int3(r < int3(0));
}

internal uint ap_packSectionIndex(int3 coords, int3 diameter) {
    coords.y += diameter.z;

    uint3 flatten = uint3(1u, diameter.x, diameter.x * diameter.y);
    uint3 sum = uint3(ap_imod(coords, diameter.xyx)) * flatten;
    return sum.x + sum.y + sum.z;
}

internal uint iris_getBlockIndex(int3 pos) {
    return ((pos.z & 15) * 16 * 16) + ((pos.y & 15) * 16) + (pos.x & 15);
}

struct Voxel {
    static Voxel EMPTY = Voxel(int4(0));
    int4 data;

    __init(int4 d) {
        data = d;
    }

    property int blockId { get { return data.x; }}
    property BlockData block { get { return ap.blocks[data.x]; }}

    // from my understanding, the cast is needed to get a logical right shift
    property int2 lightData { get { return int2(uint(data.y) >> 4 & 15, uint(data.y) >> 20 & 15); }}
}

struct VoxelBuffer {
    __subscript(int3 pos)-> Voxel {
        get {
            uint sect = ap_packSectionIndex(pos >> 4, ApertureData.internal_ChunkDiameter.xyz);

            uint sectionIndex = iris_VoxelIndices[sect];
            if (sectionIndex == uint(-1)) return Voxel.EMPTY;

            return Voxel(iris_VoxelData[sectionIndex].blocks[iris_getBlockIndex(pos)]);
        }
    }

    ChunkSection getChunk(int3 chunkPos) {
        uint sect = ap_packSectionIndex(chunkPos, ApertureData.internal_ChunkDiameter.xyz);
        uint sectionIndex = iris_VoxelIndices[sect];
        if (sectionIndex == uint(-1)) return ChunkSection.EMPTY;

        return iris_VoxelData[sectionIndex];
    }
}

struct PointLight {
    private float3 pos;
    private int blocks;

    property float3 position { get { return pos; } }
    property int block { get { return blocks; } }
}

struct PointLights {
    __subscript(uint id)->PointLight {
        get {
            return iris_PointLightBuffer[id];
        }
    }

    property uint count { get { return ApertureData.lights_PointLightCount; } }
    property float nearPlane { get { return ApertureData.lights_NearPlane; } }
    property float farPlane { get { return ApertureData.lights_FarPlane; } }
    property float4x4 projection { get { return ApertureData.lights_Projection; } }
}

struct ApertureUniformData {

    property CameraData camera { get { return CameraData(); } }
    property CelestialData celestial { get { return CelestialData(); } }
    property WorldData world { get { return WorldData(); } }
    property BlockBuffer blocks { get { return BlockBuffer(); } }
    property PointLights lights { get { return PointLights(); } }
    property GameData game { get { return GameData(); } }
    property TemporalData temporal { get { return TemporalData(); } }
    property TimeData timing { get { return TimeData(); } }
    property VoxelBuffer voxel { get { return VoxelBuffer(); }}
}
struct WorldData {
    property int time { get { return ApertureData.world_Time; } }
    property int renderDistance { get { return ApertureData.world_RenderDistance; } }
    property float rainStrength { get { return ApertureData.world_RainStrength; } }
    property float4 fogColor { get { return ApertureData.world_FogColor; } }
    property float4 skyColor { get { return ApertureData.world_SkyColor; } }
    property float fogStart { get { return ApertureData.world_FogStart; } }
    property float fogEnd { get { return ApertureData.world_FogEnd; } }
}

struct BlockData {
    private uint4 data;

    __init(uint4 d) {
        data = d;
    }

    property uint customId { get { return data.y; } }
    property float4 lightColor { get { return unpackUnorm4x8ToFloat(data.w).abgr; } }
    property uint emission { get { return int(((data.x >> 6u) & 0xfu)); } }
    property uint metadata { get { return data.x; } }
    property bool isFullBlock { get { return (data.x & 63u) == 63u; } }
    property bool isFluid { get { return (((data.x >> 18u) & 0x1u) != 0u); } }

    bool hasTag(uint tag) {
        return (data.z & (1u << tag)) != 0;
    }
}

public static const ApertureUniformData ap = ApertureUniformData();

extension<T> Optional<T> {
    T orElse(T other) {
        return select(hasValue, value, other);
    }
}

interface IVertex {
    float4x4 projectionMatrix();

    float4x4 viewMatrix();

    float3x3 normalMatrix();

    float4 pos();

    float2 uv();

    Optional<float2> midCoord() {
        return none;
    }

    float2 light();

    float4 color();

    float ao();

    int currentInstance();

    Optional<float4> overlayColor();

    Optional<float3> normal();

    Optional<float4> tangent();

    Optional<uint> blockId();

    Optional<float3> midBlock();
}

interface ISampler {
    static float4 __subscript(int2 uv);
    static float4 Sample(float2 uv);
    static float4 SampleLevel(float2 uv, float level);
    static float4 SampleBias(float2 uv, float bias);
    static float4 SampleGrad(float2 uv, float2 ddx, float2 ddy);
    static float CalculateLevelOfDetail(float2 pos);
    static void GetDimensions(uint mipLevel, out float width, out float height);
}

extern static const bool iris_isCutout = false;

float4 iris_discardIfNeeded(float4 col) {
    if (iris_isCutout) {
        if (col.a < 0.1) {
            discard;
        }
    }
    return col;
}

extension<T : ITexelElement> Sampler2D<T> {
    T sample(float2 uv) {
        return this.Sample(uv);
    }

    T sampleLevel(float2 uv, float level) {
        return this.SampleLevel(uv, level);
    }

    T sampleGradient(float2 uv, float2 ddx, float2 ddy) {
        return this.SampleGrad(uv, ddx, ddy);
    }
}

extension<T : ITexelElement> RWTexture2D<T> {
    void store(int2 uv, T value) {
        this.Store(uv, value);
    }

    T load(int2 uv) {
        return this.Load(uv);
    }
}

extern struct Vertex : IVertex;
extern struct albedoTex : ISampler;
extern struct normalTex : ISampler;
extern struct specularTex : ISampler;
extern struct lightmap : ISampler;