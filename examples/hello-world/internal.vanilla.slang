module vanilla;
import aperture;

float3 getTransform() {
    __requirePrelude(R"(
            #ifndef THE_GAME
            #define THE_GAME
            layout(std140) uniform DynamicTransforms {
                                mat4 ModelViewMat;
                                vec4 ColorModulator;
                                vec3 ModelOffset;
                                mat4 TextureMat;
                                float LineWidth;
                            } irisInt_transforms;
            #endif
        )");
    __requirePrelude(R"(
            vec3 getReg()
            {
                return irisInt_transforms.ModelOffset;
            }
        )");
    __intrinsic_asm R"(getReg)";
}

float4 getColor() {
    __requirePrelude(R"(
            #ifndef THE_GAME
            #define THE_GAME
            layout(std140) uniform DynamicTransforms {
                                mat4 ModelViewMat;
                                vec4 ColorModulator;
                                vec3 ModelOffset;
                                mat4 TextureMat;
                                float LineWidth;
                            } irisInt_transforms;
            #endif
        )");
    __requirePrelude(R"(
            vec4 getCol()
            {
                return irisInt_transforms.ColorModulator;
            }
        )");
    __intrinsic_asm R"(getCol)";
}

float4x4 getMod() {
    __requirePrelude(R"(
            #ifndef THE_GAME
            #define THE_GAME
            layout(std140) uniform DynamicTransforms {
                                mat4 ModelViewMat;
                                vec4 ColorModulator;
                                vec3 ModelOffset;
                                mat4 TextureMat;
                                float LineWidth;
                            } irisInt_transforms;
            #endif
        )");
    __requirePrelude(R"(
            mat4 getMM()
            {
                return irisInt_transforms.ModelViewMat;
            }
        )");
    __intrinsic_asm R"(getMM)";
}

float3x3 getNormalMat() {
    __requirePrelude(R"(
            uniform mat3 irisInt_normalMatrix;
    )");
    __requirePrelude(R"(
            mat3 getNM()
            {
                return irisInt_normalMatrix;
            }
        )");
    __intrinsic_asm R"(getNM)";
}

uint getCurrentCascade()
{
    __requirePrelude("uniform int iris_currentCascade;");
    __requirePrelude(R"(
            uint getCascade()
            {
                return iris_currentCascade;
            }
        )");
    __intrinsic_asm R"(getCascade)";
}

uint getCurrentPointLight()
{
    __requirePrelude("uniform int iris_currentPointLight;");
    __requirePrelude(R"(
            uint getPointLight()
            {
                return iris_currentPointLight;
            }
        )");
    __intrinsic_asm R"(getPointLight)";
}

int getCurrentPointDirections() {
    __requirePrelude("uniform int iris_currentPointDirections;");
    __requirePrelude(R"(
            int getCPD()
            {
                return iris_currentPointDirections;
            }
        )");
    __intrinsic_asm R"(getCPD)";
}

int getInstanceID() {
    __requirePrelude(R"(
            int getCI()
            {
                return gl_InstanceID;
            }
        )");
    __intrinsic_asm R"(getCI)";
}

__glsl_extension(GL_ARB_shader_viewport_layer_array)
public void setLayer(int m)
{
    __requirePrelude(R"(
            void setGLLayer(int layer)
            {
                gl_Layer = layer;
            }
        )");
    __target_switch
    {
    case glsl: __intrinsic_asm "setGLLayer";
    }
}

float4x4 getTextureMat() {
    __requirePrelude(R"(
            #ifndef THE_GAME
            #define THE_GAME
            layout(std140) uniform DynamicTransforms {
                                mat4 ModelViewMat;
                                vec4 ColorModulator;
                                vec3 ModelOffset;
                                mat4 TextureMat;
                                float LineWidth;
                            } irisInt_transforms;
            #endif
        )");
    __requirePrelude(R"(
            mat4 getTMN()
            {
                return irisInt_transforms.TextureMat;
            }
        )");
    __intrinsic_asm R"(getTMN)";
}

float4x4 getProj() {
    __requirePrelude(R"(
            #ifndef THE_GAME2
            #define THE_GAME2
            layout(std140) uniform Projection {
    mat4 ProjMat;
} iris_proj;
            #endif
        )");
    __requirePrelude(R"(
            mat4 getPM()
            {
                return iris_proj.ProjMat;
            }
        )");
    __intrinsic_asm R"(getPM)";
}

bool4 getFormatElements() {
    __requirePrelude(R"(
            uniform bvec4 irisInt_FormatElements = bvec4(true);
        )");
    __requirePrelude(R"(
            bvec4 getForma()
            {
                return irisInt_FormatElements;
            }
        )");
    __intrinsic_asm R"(getForma)";
}

bool4 getFormatElements2() {
    __requirePrelude(R"(
            uniform bvec4 irisInt_FormatElements2 = bvec4(true);
        )");
    __requirePrelude(R"(
            bvec4 getForma2()
            {
                return irisInt_FormatElements2;
            }
        )");
    __intrinsic_asm R"(getForma2)";
}
extern static const bool iris_isCSM;
extern static const bool iris_isPointLight;

static const float4x4 iris_rotation[6] = {
    // +X
    float4x4(
        0.0, 0.0, -1.0, 0.0, 
        0.0, -1.0, 0.0, 0.0,
        -1.0, 0.0, 0.0, 0.0, 
        0.0, 0.0, 0.0, 1.0  
        ),
    // –X
    float4x4(
        0.0, 0.0, 1.0, 0.0,
        0.0, -1.0, 0.0, 0.0,
        1.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    ),

    // +Y
    float4x4(
        1.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, -1.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    ),
    // –Y
    float4x4(
        1.0, 0.0, 0.0, 0.0,
        0.0, 0.0, -1.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    ),
    // +Z
    float4x4(
        1.0, 0.0, 0.0, 0.0,
        0.0, -1.0, 0.0, 0.0,
        0.0, 0.0, -1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    ),
    // –Z
    float4x4(
        -1.0, 0.0, 0.0, 0.0,
        0.0, -1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    )
};


struct VanillaVertex : IVertex {
    [[vk::location(0)]] float3 a_pos;
    [[vk::location(1)]] float4 a_color;
    [[vk::location(2)]] float2 a_uv;
    [[vk::location(3)]] int2 uv1;
    [[vk::location(4)]] int2 uv2;
    [[vk::location(5)]] float3 a_normal;
    [[vk::location(6)]] float4 a_tangent;
    [[vk::location(7)]] float2 a_midCoord;

    int currentInstance() {
        if (iris_isCSM) {
            return getCurrentCascade();
        } else if (iris_isPointLight) {
            return getCurrentPointLight();
        } else {
            return 0;
        }
    }

    float ao() {
        return 1.0;
    }

    float4x4 projectionMatrix() {
        return transpose(getProj());
    }

    float3x3 normalMatrix() {
        return transpose(getNormalMat());
    }
    
    float4x4 viewMatrix() {
        if (iris_isPointLight) {
            int dirs = (getCurrentPointDirections() >> (getInstanceID() * 4)) & 0xF;
            return iris_rotation[dirs];
        } else {
            return transpose(getMod());
        }
    }

    float4 color() {
        return (getFormatElements().x ? a_color : float4(1.0)) * getColor();
    }

    float4 pos() {
        if (iris_isCSM) {
            setLayer(getCurrentCascade());
        } else if (iris_isPointLight) {
            int dirs = (getCurrentPointDirections() >> (getInstanceID() * 4)) & 0xF;
            setLayer((getCurrentPointLight() * 6) + dirs);
        }
        return float4(a_pos + getTransform(), 1.0);
    }

    Optional<float4> overlayColor() {
        return select(getFormatElements2().x, overlayTex[uv1], none);
    }

    Optional<float3> normal() {
        return select(getFormatElements().y, a_normal, none);
    }

    Optional<float4> tangent() {
        return select(getFormatElements2().y, a_tangent, none);
    }

    override Optional<float2> midCoord() {
        return select(getFormatElements2().z, a_midCoord, float2(0.0, 0.0)); // TODO
    }

    Optional<uint> blockId() {
        return none; // TODO
    }

    Optional<float3> midBlock() {
        return none; // TODO
    }

    float2 uv() {
        return getFormatElements().w ? mul(transpose(getTextureMat()), float4(a_uv, 0.0, 1.0)).xy : float2(1.0);
    }

    float2 light() {
        return getFormatElements().z ? clamp(uv2 / 256.0, float2(0.5 / 16.0), float2(15.5 / 16.0)) : float2(1.0);
    }
}

[[vk::binding(0)]]
Sampler2D worldSampler;

[[vk::binding(1)]]
Sampler2D overlayTex;

[[vk::binding(2)]]
Sampler2D lightmapS;

Sampler2D irisInt_normalSampler;
Sampler2D irisInt_specularSampler;

struct DefaultSampler : ISampler {
    float4 c;

    static float4 __subscript(int2 uv) {
        return getFormatElements().w ? worldSampler[uv] : float4(1.0); 
    }

    static float4 Sample(float2 uv) {
        return getFormatElements().w ? worldSampler.Sample(uv) : float4(1.0);
    }

    static float4 SampleLevel(float2 uv, float level) {
        return getFormatElements().w ? worldSampler.SampleLevel(uv, level) : float4(1.0);
    }

    static float4 SampleBias(float2 uv, float bias) {
        return getFormatElements().w ? worldSampler.SampleBias(uv, bias) : float4(1.0);
    }

    static float4 SampleGrad(float2 uv, float2 ddx, float2 ddy) {
        return getFormatElements().w ? worldSampler.SampleGrad(uv, ddx, ddy) : float4(1.0);
    }

    static float CalculateLevelOfDetail(float2 pos) {
        return getFormatElements().w ? worldSampler.CalculateLevelOfDetail(pos) : 0.0;
    }

    static void GetDimensions(uint mipLevel, out float width, out float height) {
        float numLevels;
        worldSampler.GetDimensions(mipLevel, width, height, numLevels);
    }
}

struct NormalSampler : ISampler {
    float4 c1;

    static float4 __subscript(int2 uv) {
        return irisInt_normalSampler[uv];
    }

    static float4 Sample(float2 uv) {
        return irisInt_normalSampler.Sample(uv);
    }

    static float4 SampleLevel(float2 uv, float level) {
        return irisInt_normalSampler.SampleLevel(uv, level);
    }

    static float4 SampleBias(float2 uv, float bias) {
        return irisInt_normalSampler.SampleBias(uv, bias);
    }

    static float4 SampleGrad(float2 uv, float2 ddx, float2 ddy) {
        return irisInt_normalSampler.SampleGrad(uv, ddx, ddy);
    }

    static float CalculateLevelOfDetail(float2 pos) {
        return getFormatElements().w ? irisInt_normalSampler.CalculateLevelOfDetail(pos) : 0.0;
    }
    static void GetDimensions(uint mipLevel, out float width, out float height) {
        float numLevels;
        irisInt_normalSampler.GetDimensions(mipLevel, width, height, numLevels);
    }
}

struct SpecularSampler : ISampler {
    float4 c1;

    static float4 __subscript(int2 uv) {
        return irisInt_specularSampler[uv];
    }

    static float4 Sample(float2 uv) {
        return irisInt_specularSampler.Sample(uv);
    }

    static float4 SampleLevel(float2 uv, float level) {
        return irisInt_specularSampler.SampleLevel(uv, level);
    }

    static float4 SampleGrad(float2 uv, float2 ddx, float2 ddy) {
        return irisInt_specularSampler.SampleGrad(uv, ddx, ddy);
    }

    static float4 SampleBias(float2 uv, float bias) {
        return irisInt_specularSampler.SampleBias(uv, bias);
    }

    static float CalculateLevelOfDetail(float2 pos) {
        return getFormatElements().w ? irisInt_specularSampler.CalculateLevelOfDetail(pos) : 0.0;
    }

    static void GetDimensions(uint mipLevel, out float width, out float height) {
        float numLevels;
        irisInt_specularSampler.GetDimensions(mipLevel, width, height, numLevels);
    }
}


struct LightmapSampler : ISampler {
    float4 c2;

    static float4 __subscript(int2 uv) {
        return getFormatElements().z ? lightmapS[uv] : float4(1.0);
    }

    static float4 Sample(float2 uv) {
        return getFormatElements().z ? lightmapS.Sample(uv) : float4(1.0);
    }

    static float4 SampleLevel(float2 uv, float level) {
        return getFormatElements().z ? lightmapS.SampleLevel(uv, level) : float4(1.0);
    }

    static float4 SampleGrad(float2 uv, float2 ddx, float2 ddy) {
        return getFormatElements().z ? lightmapS.SampleGrad(uv, ddx, ddy) : float4(1.0);
    }

    static float4 SampleBias(float2 uv, float bias) {
        return getFormatElements().z ? lightmapS.SampleBias(uv, bias) : float4(1.0);
    }

    static float CalculateLevelOfDetail(float2 pos) {
        return getFormatElements().z ? lightmapS.CalculateLevelOfDetail(pos) : 0.0;
    }

    static void GetDimensions(uint mipLevel, out float width, out float height) {
        float numLevels;
        lightmapS.GetDimensions(mipLevel, width, height, numLevels);
    }
}

export struct albedoTex : ISampler = DefaultSampler;
export struct normalTex : ISampler = NormalSampler;
export struct specularTex : ISampler = SpecularSampler;
export struct lightmap : ISampler = LightmapSampler;

export struct Vertex : IVertex = VanillaVertex;
export static const bool shouldAlphaTest = true;
