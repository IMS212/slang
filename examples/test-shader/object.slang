import engine;

Sampler2D mainTexture;

struct RealSurface : Surface {
    float4 position : SV_Position;
    float4 worldPos;
    float2 uvCoord;

    float4 pos() {
        return worldPos;
    }

    float2 uv() {
        return uvCoord;
    }

    float4 albedo() {
        return mainTexture.Sample(uvCoord);
    }
}

struct RealVertex : Vertex {
    float4 position;
    float2 uvCoord;
    typedef RealSurface Surf;

    float4 pos() {
        return position;
    }

    float2 uv() {
        return uvCoord;
    }

    This setPos(float4 pos) {
        return RealVertex(pos, uvCoord);
    }

    RealSurface toSurface() {
        RealSurface s;
        s.position = position;
        s.worldPos = position;
        s.uvCoord = uvCoord;
        return s;
    }
}

struct RealVertexInput : VertexInput {
    [[vk::location(0)]] float4 position;
    [[vk::location(1)]] float2 uvCoord;

    typedef RealVertex Vtx;

    Vtx toVertex() {
        return RealVertex(position, uvCoord);
    }
}