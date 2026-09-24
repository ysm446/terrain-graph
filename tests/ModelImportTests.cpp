#include <cmath>
#include <fstream>
#include <iostream>

#include "renderer/ModelAsset.h"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    using tg::renderer::LoadModel;
    using tg::renderer::ModelAsset;
    if (argc > 1) {
        for (const auto& file : fs::directory_iterator(fs::path(argv[1]))) {
            if (file.path().extension() != L".FBX") continue;
            ModelAsset asset;
            if (!LoadModel(file.path(), asset)) {
                std::cerr << asset.error;
                return 1;
            }
            const auto& g = *asset.geometry;
            std::cout << file.path().filename() << " LODs=" << g.lods.size()
                      << " slots=" << g.slots.size()
                      << " dimensions(m)=" << g.maximum.x - g.minimum.x << ","
                      << g.maximum.y - g.minimum.y << "," << g.maximum.z - g.minimum.z
                      << " triangles=";
            for (const auto& lod : g.lods) std::cout << lod.triangles << " ";
            std::cout << "\n";
            if (g.lods.size() != 4 || g.slots.size() != 1 || g.maximum.x - g.minimum.x > 0.5f)
                return 2;
        }
        return 0;
    }
    const auto path = fs::temp_directory_path() / L"terrain_graph_model_import_test.fbx";
    {
        std::ofstream out(path);
        out << R"(; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
FBXVersion: 7400
}
GlobalSettings: {
Version: 1000
Properties70: {
P: "UpAxis", "int", "Integer", "",2
P: "UpAxisSign", "int", "Integer", "",1
P: "FrontAxis", "int", "Integer", "",1
P: "FrontAxisSign", "int", "Integer", "",-1
P: "CoordAxis", "int", "Integer", "",0
P: "CoordAxisSign", "int", "Integer", "",1
P: "UnitScaleFactor", "double", "Number", "",1
}
}
Objects: {
Geometry: 1, "Geometry::Triangle", "Mesh" {
Vertices: *9 { a: 0,0,0,100,0,0,0,0,100 }
PolygonVertexIndex: *6 { a: 0,1,-3,0,1,-3 }
LayerElementMaterial: 0 {
Version: 101
Name: ""
MappingInformationType: "ByPolygon"
ReferenceInformationType: "IndexToDirect"
Materials: *2 { a: 0,1 }
}
LayerElementUV: 0 {
Version: 101
Name: "UV0"
MappingInformationType: "ByPolygonVertex"
ReferenceInformationType: "IndexToDirect"
UV: *6 { a: 0,0,1,0,0,1 }
UVIndex: *6 { a: 0,1,2,0,1,2 }
}
Layer: 0 {
Version: 100
LayerElement: { Type: "LayerElementMaterial"
TypedIndex: 0
}
LayerElement: { Type: "LayerElementUV"
TypedIndex: 0
}
}
}
Material: 3, "Material::SameName", "" { Version: 102 }
Material: 4, "Material::SameName", "" { Version: 102 }
Model: 2, "Model::Triangle", "Mesh" {
Version: 232
Properties70: {
P: "Lcl Translation", "Lcl Translation", "", "A",200,0,0
}
}
}
Connections: {
C: "OO",1,2
C: "OO",2,0
C: "OO",3,2
C: "OO",4,2
}
)";
    }
    ModelAsset asset;
    if (!LoadModel(path, asset)) {
        std::cerr << asset.error;
        return 1;
    }
    const auto& g = *asset.geometry;
    if (g.lods.size() != 1 || g.lods[0].triangles != 2) return 2;
    if (g.slots.size() != 2 || g.lods[0].parts.size() != 2) return 6;
    const auto& mesh = g.lods[0].parts[0].mesh;
    if (mesh.vertices.size() != 3 || std::abs(mesh.vertices[0].uv.y - 1.0f) > 1e-5f) return 7;
    // cm→m、Z-up→Y-up、ノードの移動を同時に検証する。
    if (std::abs(g.minimum.x - 2) > 1e-5f || std::abs(g.maximum.x - 3) > 1e-5f ||
        std::abs(g.maximum.y - g.minimum.y - 1) > 1e-5f ||
        std::abs(g.maximum.z - g.minimum.z) > 1e-5f)
        return 3;
    for (const auto& vertex : g.lods[0].parts[0].mesh.vertices) {
        const auto n = vertex.normal;
        if (!std::isfinite(n.x + n.y + n.z) ||
            std::abs(n.x * n.x + n.y * n.y + n.z * n.z - 1) > 1e-4f)
            return 4;
    }
    {
        // LODGroup の無い FBX でも、名前の末尾 _LOD<n> で段を分ける。番号の飛びは詰める。
        std::ofstream out(path);
        out << R"(; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
FBXVersion: 7400
}
GlobalSettings: {
Version: 1000
Properties70: {
P: "UpAxis", "int", "Integer", "",1
P: "UpAxisSign", "int", "Integer", "",1
P: "FrontAxis", "int", "Integer", "",2
P: "FrontAxisSign", "int", "Integer", "",1
P: "CoordAxis", "int", "Integer", "",0
P: "CoordAxisSign", "int", "Integer", "",1
P: "UnitScaleFactor", "double", "Number", "",100
}
}
Objects: {
Geometry: 10, "Geometry::Detail", "Mesh" {
Vertices: *12 { a: 0,0,0,2,0,0,0,1,0,2,1,0 }
PolygonVertexIndex: *6 { a: 0,1,-3,1,3,-3 }
}
Geometry: 11, "Geometry::Coarse", "Mesh" {
Vertices: *9 { a: 0,0,0,2,0,0,0,1,0 }
PolygonVertexIndex: *3 { a: 0,1,-3 }
}
Model: 20, "Model::Bush_LOD0", "Mesh" { Version: 232 }
Model: 21, "Model::Bush_lod2", "Mesh" { Version: 232 }
}
Connections: {
C: "OO",10,20
C: "OO",20,0
C: "OO",11,21
C: "OO",21,0
}
)";
    }
    {
        ModelAsset named;
        if (!LoadModel(path, named)) {
            std::cerr << named.error;
            return 8;
        }
        const auto& lods = named.geometry->lods;
        if (lods.size() != 2 || lods[0].triangles != 2 || lods[1].triangles != 1) return 9;
        // 既定は最大寸法（2 m）の 10 倍、次の段は 3 倍。設定があればそれを使う。
        using tg::renderer::LodStartDistance;
        if (LodStartDistance(named, 0) != 0 || std::abs(LodStartDistance(named, 1) - 20) > 1e-4f ||
            std::abs(LodStartDistance(named, 2) - 60) > 1e-4f)
            return 10;
        named.lodDistances = {5};
        if (LodStartDistance(named, 1) != 5 || std::abs(LodStartDistance(named, 2) - 60) > 1e-4f) return 11;
    }
    {
        std::ofstream out(path);
        out << "invalid";
    }
    ModelAsset invalid;
    if (LoadModel(path, invalid) || invalid.error.empty()) return 5;
    std::error_code ec;
    fs::remove(path, ec);
    std::cout << "Model import tests passed\n";
    return 0;
}
