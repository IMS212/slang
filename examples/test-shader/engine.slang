public module engine;

interface VertexInput {
    associatedtype Vtx : Vertex;
    Vtx toVertex();
}

interface Vertex {
    float4 pos();
    float2 uv();
    associatedtype Surf : Surface;

    This setPos(float4 pos);

    Surf toSurface();
}

interface Surface {
    float4 pos();
    float2 uv();
    float4 albedo();
}

interface ShaderOut {}

interface Shader<O : ShaderOut>
{
    static float4 deformVertex(Vertex v);
    static O shadeSurface(Surface s);
}

[shader("vertex")]
V.Surf vertexMain<In:VertexInput, V:Vertex, S, O:ShaderOut>(In input)
    where V == In.Vtx
    where S : Shader<O>
{
    V v = input.toVertex();
    v = v.setPos(S.deformVertex(v));
    return v.toSurface();
}

[shader("fragment")]
O fragmentMain<V:Vertex, S, O:ShaderOut>(V.Surf surf)
    where S : Shader<O>
{
    return S.shadeSurface(surf);
}