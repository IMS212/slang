import engine;

Sampler2D mainTexture;

struct RealSurface : Surface {
    float4 position : SV_Position;
    float4 worldPos;
    float2 uvCoord;
    float3 normalVec;

    float4 pos() {
        return worldPos;
    }

    float2 uv() {
        return uvCoord;
    }

    float4 albedo() {
        return mainTexture.Sample(uvCoord);
    }

    float3 normals() {
        return normalVec;
    }
}

struct RealVertex : Vertex {
    float4 position;
    float2 uvCoord;
    float3 normalVec;
    float2 lightUV;
    uint block;
    uint vID;
    typedef RealSurface Surf;

    float4 pos() {
        return position;
    }

    float2 uv() {
        return uvCoord;
    }

    float3 normals() {
        return normalVec;
    }

    float2 lightCoord() {
        return lightUV;
    }

    uint blockID() {
        return block;
    }

    uint vertexID() {
        return vID;
    }

    This setPos(float4 pos) {
        return RealVertex(pos, uvCoord, normalVec, lightUV, block, vID);
    }

    RealSurface toSurface() {
        RealSurface s;
        s.position = position;
        s.worldPos = position;
        s.uvCoord = uvCoord;
        s.normalVec = normalVec;
        return s;
    }
}

struct RealVertexInput : VertexInput {
    [[vk::location(0)]] float4 position;
    [[vk::location(1)]] float2 uvCoord;
    [[vk::location(2)]] float3 normalVec;
    [[vk::location(3)]] float2 lightUV;
    [[vk::location(4)]] uint blockID;

    typedef RealVertex Vtx;

    Vtx toVertex(uint vID) {
        return RealVertex(position, uvCoord, normalVec, lightUV, blockID, vID);
    }
}