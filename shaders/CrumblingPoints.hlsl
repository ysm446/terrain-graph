// Pointsだけを処理する。最大直径をセル幅にした空間ハッシュで近傍を調べる。
// 元の候補は読み取り専用。小さい粒子IDを優先するので実行順によらない。
struct PointConstants {
    uint source, output, grid, count;
    uint rows, avoidOverlap; float cellSize; uint padding;
};
ConstantBuffer<PointConstants> g_points : register(b1);
uint2 Address(uint index) { return uint2(index % 1024, index / 1024); }
uint CellHash(int2 cell) {
    uint h = asuint(cell.x)*73856093u ^ asuint(cell.y)*19349663u;
    h ^= h >> 16;
    return h & 262143u;
}
[numthreads(64,1,1)]
void CsClear(uint3 id : SV_DispatchThreadID) {
    RWTexture2D<uint> grid = ResourceDescriptorHeap[g_points.grid];
    if (id.x < 262144) grid[Address(id.x)] = 0xffffffffu;
    if (id.x == 0) grid[uint2(0,256+g_points.rows)] = 0;
}
[numthreads(64,1,1)]
void CsBuild(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_points.count) return;
    Texture2D<float4> source = ResourceDescriptorHeap[g_points.source];
    RWTexture2D<uint> grid = ResourceDescriptorHeap[g_points.grid];
    float4 candidate = source.Load(int3(Address(id.x),0));
    if (candidate.w <= 0) return;
    uint previous;
    uint bucket = CellHash(int2(floor(candidate.xz/g_points.cellSize)));
    InterlockedExchange(grid[Address(bucket)],id.x,previous);
    grid[Address(id.x)+uint2(0,256)] = previous;
}
[numthreads(64,1,1)]
void CsFilter(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_points.count) return;
    Texture2D<float4> source = ResourceDescriptorHeap[g_points.source];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_points.output];
    RWTexture2D<uint> grid = ResourceDescriptorHeap[g_points.grid];
    uint2 address = Address(id.x);
    float4 candidate = source.Load(int3(address,0));
    bool keep = candidate.w > 0;
    if (keep && g_points.avoidOverlap != 0) {
        int2 cell = int2(floor(candidate.xz/g_points.cellSize));
        for (int y=-1;y<=1 && keep;++y) for (int x=-1;x<=1 && keep;++x) {
            uint neighbor = grid[Address(CellHash(cell+int2(x,y)))];
            while (neighbor != 0xffffffffu) {
                if (neighbor < id.x) {
                    float4 other = source.Load(int3(Address(neighbor),0));
                    float2 delta = candidate.xz-other.xz;
                    float separation = (candidate.w+other.w)*0.5;
                    if (dot(delta,delta) < separation*separation) { keep=false; break; }
                }
                neighbor = grid[Address(neighbor)+uint2(0,256)];
            }
        }
    }
    output[address] = keep ? candidate : 0;
    output[address+uint2(0,g_points.rows)] = source.Load(int3(address+uint2(0,g_points.rows),0));
    if (keep) InterlockedAdd(grid[uint2(0,256+g_points.rows)],1);
}
