#pragma once
#include "graph/LayerMaterial.h"
#include <nlohmann/json.hpp>
#include <functional>
namespace tg::io {
nlohmann::json WriteLayerMaterial(const graph::LayerMaterial& material);
bool ReadLayerMaterial(const nlohmann::json& body, graph::LayerMaterial& material, std::string& error);
void MapLayerMaterials(nlohmann::json& body, const std::function<nlohmann::json(const nlohmann::json&)>& convert);
}
